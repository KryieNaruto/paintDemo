#include "sdk_api/dgc_paint_c_api.h"

#include <array>
#include <memory>
#include <unordered_map>

#include "core/determinism.h"
#include "core/engine.h"
#include "core/interfaces/i_paint_kernel.h"
#include "core/interfaces/i_render_backend.h"
#include "core/stroke_predictor.h"
#include "core/types.h"
#include "kernels/brush/brush_kernel_factory.h"
#ifdef DGCPAIN_HAVE_BRUSH
#include "kernels/brush/brush_kernel.h"
#endif
#include "render/render_backend_factory.h"

// DgcContext 为不透明句柄，内部定义不对外暴露。经典 Pimpl：唯一数据成员是
// std::unique_ptr<Impl>，Impl 定义留在本 cpp 内。kernel/backend 用 unique_ptr 持有，
// 便于 B2-1/B3-1 落地真实后端/内核时替换（现为 Null 桩）。
struct DgcContext {
    struct Impl;
    std::unique_ptr<Impl> impl_;
    ~DgcContext();
};

struct DgcContext::Impl {
    // 声明序 = kernel → backend → engine：C++ 逆声明序析构 = engine（先 stop/join 三线程，
    // 仍持有 kernel/backend 存活）→ backend → kernel，与既有释放顺序一致。
    std::unique_ptr<IPaintKernel>   kernel;
    std::unique_ptr<IRenderBackend> backend;
    std::unique_ptr<Engine>         engine;
    int w = 0;
    int h = 0;
    // B1-7 新增：确定性机制（core/determinism.h）。B1-6 的随机种子/固定时间存参已收敛
    // 到 DeterminismState（seed/fixed_time_us/override_time）。rng 由 unique_ptr 持有，
    // determinism/time_stepper 为值成员；声明序在 engine 之后（rng 为纯值型随机源，
    // 与线程无交互，析构顺序无敏感点）。
    std::unique_ptr<IRandomSource> rng;         // RAII 持有；dgcSetRandomSeed 重建/重播种
    DeterminismState determinism;               // §4.0.3 值成员
    FixedTimeStepper  time_stepper;             // 固定时间步进
    std::unordered_map<DgcBrush, std::array<double, DGC_SETTING_COUNT>> brush_settings;
    std::unordered_map<DgcBrush, std::array<float, 4>>                  brush_colors;

    // D6-1：context 级 stroke modeler 参数与惰性激活的预测器。
    //   - model_params 为值成员，缺省即 StrokeModelParams 的字面默认值；
    //     dgcSetBrushSetting(modeler 项) 改写后立即 Configure 生效。
    //   - predictor_handle_ 是**非拥有**裸指针：真正所有权在
    //     engine->setPredictor 移交后的 Engine::predictor_（unique_ptr），
    //     本处只保留一份指针用于后续 Configure 调用，不 new/delete、不重复释放。
    //   - 惰性激活（plan §4.2）：dgcCreate 不创建/注入 predictor（passthrough，
    //     默认零回归）；首次设置任一 modeler settingId 才 make_unique 创建、
    //     Configure、经 engine->setPredictor 移交所有权。
    StrokeModelParams model_params;
    StrokeModeler*    predictor_handle_ = nullptr;  // 非拥有裸指针，不释放
    uint64_t          next_brush_handle_ = 1;       // dgcCreateBrush 发号器
};

// 析构器必须在 Impl 完整后定义（unique_ptr<Impl> + 不完整类型，否则隐式析构在声明处
// 实例化会因 incomplete type 编译失败）。
DgcContext::~DgcContext() = default;

namespace {

// 错误码记录：dgcGetLastError 无 ctx 参数，故用线程局部变量。
thread_local DgcError g_last_error = DGC_OK;

// D6-1：settingId >= DGC_SETTING_WOBBLE_TIMEOUT_MS 即 stroke modeler 参数
// （见 sdk_api/dgc_paint_c_api.h 枚举分段注释）；写入 StrokeModelParams 对应字段。
// 调用方需先保证 settingId 已在 [0, DGC_SETTING_COUNT) 范围内。
void applyModelerSetting(StrokeModelParams& p, int settingId, double value) {
    switch (settingId) {
        case DGC_SETTING_WOBBLE_TIMEOUT_MS:
            p.wobble_timeout_ms = static_cast<float>(value);
            break;
        case DGC_SETTING_WOBBLE_SPEED_FLOOR:
            p.wobble_speed_floor = static_cast<float>(value);
            break;
        case DGC_SETTING_MIN_OUTPUT_RATE_HZ:
            p.min_output_rate_hz = static_cast<float>(value);
            break;
        case DGC_SETTING_END_OF_STROKE_STOPPING_DISTANCE_MM:
            p.end_of_stroke_stopping_distance_mm = static_cast<float>(value);
            break;
        case DGC_SETTING_SPRING_MASS_CONSTANT:
            p.spring_mass_constant = static_cast<float>(value);
            break;
        case DGC_SETTING_SPRING_DRAG_CONSTANT:
            p.spring_drag_constant = static_cast<float>(value);
            break;
        case DGC_SETTING_KALMAN_PROCESS_NOISE:
            p.kalman_process_noise = static_cast<float>(value);
            break;
        case DGC_SETTING_KALMAN_MEASUREMENT_NOISE:
            p.kalman_measurement_noise = static_cast<float>(value);
            break;
        case DGC_SETTING_PREDICTION_INTERVAL_MS:
            p.prediction_interval_ms = static_cast<float>(value);
            break;
        default:
            break;  // 不可达：调用方已校验 settingId 属于 modeler 段。
    }
}

const char* errorMessage(DgcError e) {
    switch (e) {
        case DGC_OK:                  return nullptr;
        case DGC_ERR_NULL_CONTEXT:    return "null context";
        case DGC_ERR_INVALID_ARG:     return "invalid argument";
        case DGC_ERR_INVALID_HANDLE:  return "invalid brush handle";
        case DGC_ERR_NOT_IMPLEMENTED: return "not implemented";
        case DGC_ERR_QUEUE_FULL:      return "input queue full";
    }
    return "unknown error";
}

}  // namespace

extern "C" {

DgcContext* dgcCreate(void) {
    g_last_error = DGC_OK;
    // 全部用 unique_ptr 栈上构建：任一构造/start() 抛异常时已建对象自动析构，零泄漏。
    auto ctx  = std::make_unique<DgcContext>();
    auto impl = std::make_unique<DgcContext::Impl>();
    impl->kernel  = CreateDefaultPaintKernel();    // BrushKernel（DGCPAIN_HAVE_BRUSH）或 Null 兜底
    impl->backend = CreateDefaultRenderBackend();  // unique_ptr<IRenderBackend>
    impl->engine  = std::make_unique<Engine>(impl->kernel.get(), impl->backend.get());
    impl->rng     = std::make_unique<Mt19937Random>(0);  // 默认 seed 0
    impl->engine->start();
    ctx->impl_ = std::move(impl);
    return ctx.release();
}

void dgcDestroy(DgcContext* ctx) {
    if (ctx == nullptr) {
        return;
    }
    // 经 unique_ptr 析构器自动释放；engine->stop() 幂等，impl_.reset() 幂等。
    std::unique_ptr<DgcContext> guard(ctx);
    if (guard->impl_ && guard->impl_->engine) {
        guard->impl_->engine->stop();
    }
    guard->impl_.reset();
}

int dgcSetSurface(DgcContext* ctx, void* nativeWindow, int w, int h) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (w < 0 || h < 0) {
        g_last_error = DGC_ERR_INVALID_ARG;
        return DGC_ERR_INVALID_ARG;
    }
    ctx->impl_->w = w;
    ctx->impl_->h = h;
    // Null 后端接受 nativeWindow == NULL（headless）。
    ctx->impl_->backend->init(nativeWindow, w, h);
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcResize(DgcContext* ctx, int w, int h) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (w < 0 || h < 0) {
        g_last_error = DGC_ERR_INVALID_ARG;
        return DGC_ERR_INVALID_ARG;
    }
    ctx->impl_->w = w;
    ctx->impl_->h = h;
    ctx->impl_->backend->resize(w, h);
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcBeginStroke(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    StrokePoint p{x, y, pressure, tiltX, tiltY, 0, false};
    StrokeEvent ev{StrokeEventType::BeginStroke, p};
    if (!ctx->impl_->engine->submitInput(ev)) {
        g_last_error = DGC_ERR_QUEUE_FULL;
        return DGC_ERR_QUEUE_FULL;
    }
    // B1-7：每笔画开始把固定时间步进器归零（仅在 C API 调用线程推进，与引擎三线程
    // 无共享；fixed_time_us/override_time 在此读一次快照）。
    ctx->impl_->time_stepper.beginStroke(ctx->impl_->determinism.fixed_time_us,
                                         ctx->impl_->determinism.override_time);
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcStrokeTo(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY, int isPredicted) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    // B1-7：t_us 由固定时间步进器产出（override 时 n*step，否则 0）。
    StrokePoint p{x, y, pressure, tiltX, tiltY, ctx->impl_->time_stepper.next(), (isPredicted != 0)};
    StrokeEvent ev{StrokeEventType::StrokePoint, p};
    if (!ctx->impl_->engine->submitInput(ev)) {
        g_last_error = DGC_ERR_QUEUE_FULL;
        return DGC_ERR_QUEUE_FULL;
    }
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcEndStroke(DgcContext* ctx) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    StrokePoint p{};
    StrokeEvent ev{StrokeEventType::EndStroke, p};
    if (!ctx->impl_->engine->submitInput(ev)) {
        g_last_error = DGC_ERR_QUEUE_FULL;
        return DGC_ERR_QUEUE_FULL;
    }
    g_last_error = DGC_OK;
    return DGC_OK;
}

DgcBrush dgcCreateBrush(DgcContext* ctx, const DgcBrushParams* params) {
    (void)params;
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_INVALID_BRUSH;
    }
    // D6-1（plan §4.4，B1-4 遗留的最小落地）：仅做发号器，返回自增非 0 句柄，
    // 供 dgcSetBrushSetting/dgcSetBrushColor 校验非 DGC_INVALID_BRUSH 用；
    // 不触达 engine/kernel —— 引擎渲染仍用固定默认笔刷（笔刷切换/加载不在
    // D6-1 范围，dgcSetBrush/dgcDestroyBrush/dgcLoadBrushFromMyb 保持
    // NOT_IMPLEMENTED）。modeler 参数是 context 级单例，与具体笔刷句柄值无关。
    const DgcBrush handle = static_cast<DgcBrush>(ctx->impl_->next_brush_handle_++);
    g_last_error = DGC_OK;
    return handle;
}

DgcBrush dgcLoadBrushFromMyb(DgcContext* ctx, const char* mybJson) {
    (void)mybJson;
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_INVALID_BRUSH;
    }
    // 无 L5（libmypaint）时 myb 加载必失败。
    g_last_error = DGC_ERR_NOT_IMPLEMENTED;
    return DGC_INVALID_BRUSH;
}

int dgcDestroyBrush(DgcContext* ctx, DgcBrush brush) {
    (void)brush;
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    g_last_error = DGC_ERR_NOT_IMPLEMENTED;
    return DGC_ERR_NOT_IMPLEMENTED;
}

int dgcSetBrush(DgcContext* ctx, DgcBrush brush) {
    (void)brush;
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    g_last_error = DGC_ERR_NOT_IMPLEMENTED;
    return DGC_ERR_NOT_IMPLEMENTED;
}

int dgcRender(DgcContext* ctx) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    // 3 线程引擎下渲染线程已持续 composite + present，帧触发为 no-op。
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcClear(DgcContext* ctx, float r, float g, float b, float a) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    ctx->impl_->backend->clearCanvas(r, g, b, a);
    g_last_error = DGC_OK;
    return DGC_OK;
}

const char* dgcGetLastError(void) {
    return errorMessage(g_last_error);
}

/* ── v3.0：离屏 / 导出 / 像素读回（B2-1 接真实后端）── */

int dgcSetOffscreenSurface(DgcContext* ctx, int w, int h) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (w < 0 || h < 0) {
        g_last_error = DGC_ERR_INVALID_ARG;
        return DGC_ERR_INVALID_ARG;
    }
    if (!ctx->impl_->backend->supportsOffscreen()) {
        g_last_error = DGC_ERR_NOT_IMPLEMENTED;
        return DGC_ERR_NOT_IMPLEMENTED;
    }
    ctx->impl_->backend->initOffscreen(w, h);
    ctx->impl_->w = w;
    ctx->impl_->h = h;
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcExportPNG(DgcContext* ctx, const char* path) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (path == nullptr) {
        g_last_error = DGC_ERR_INVALID_ARG;
        return DGC_ERR_INVALID_ARG;
    }
    if (!ctx->impl_->backend->supportsOffscreen()) {
        g_last_error = DGC_ERR_NOT_IMPLEMENTED;
        return DGC_ERR_NOT_IMPLEMENTED;
    }
    // 导出前先 drain：exportPNG 内部即画布读回，引擎三线程异步合成有滞后，
    // 必须先 flush 等全部已提交输入合成完成，否则导出缺最近 dab（线条空洞）。
    // 与 dgcFlush 同逻辑；flush 幂等，无未决输入时直接返回。
    if (ctx->impl_->engine->running()) {
        ctx->impl_->engine->flush();
    }
    ctx->impl_->backend->exportPNG(path);
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcReadbackPixels(DgcContext* ctx, void* rgbaOut) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (rgbaOut == nullptr) {
        g_last_error = DGC_ERR_INVALID_ARG;
        return DGC_ERR_INVALID_ARG;
    }
    if (!ctx->impl_->backend->supportsOffscreen()) {
        g_last_error = DGC_ERR_NOT_IMPLEMENTED;
        return DGC_ERR_NOT_IMPLEMENTED;
    }
    // bugfix（20fps 回退）：不再在此 drain——早先版本在此强制 engine->flush()，把 GUI 高频
    // 调用（每帧一次）变成同步等渲染线程 composite 完成，快速甩笔时直接掉帧到 20fps，
    // 抵消了渲染线程「批量 composite」优化本该带来的异步解耦（见 docs/plans/
    // bugfix-readback-blocks-render-thread.md）。正确性改由 VkBackend::readback() 内部的
    // 快照缓存保证：缓存只在渲染线程完整 composite/clear 一批提交完成后才刷新，因此这里
    // 永远读到"完整画布"（可能比最新输入落后一批，但不会缺 dab），且是纯内存拷贝，不等
    // 渲染线程。
    ctx->impl_->backend->readback(rgbaOut);
    g_last_error = DGC_OK;
    return DGC_OK;
}

/* ── v3.0：确定性（B1-7 接线：注入/重播种 Mt19937Random + 固定时间步进）── */

int dgcSetRandomSeed(DgcContext* ctx, uint64_t seed) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    ctx->impl_->determinism.seed = seed;
    ctx->impl_->rng = std::make_unique<Mt19937Random>(seed);  // 重建（旧 unique_ptr 自动释放）
    // 把 seed 贯通到真实内核 RNG（BrushKernel::SetSeed），避免内核与 context 双随机源分叉
    // （plan-review 提醒 2：context 的 rng 是 seed 权威持有者，内核 RNG 同 seed 重播种）。
#ifdef DGCPAIN_HAVE_BRUSH
    if (auto* bk = dynamic_cast<BrushKernel*>(ctx->impl_->kernel.get())) {
        bk->SetSeed(seed);
    }
#endif
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcSetFixedTime(DgcContext* ctx, double t_us) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    ctx->impl_->determinism.fixed_time_us = t_us;
    ctx->impl_->determinism.override_time = true;
    g_last_error = DGC_OK;
    return DGC_OK;
}

/* ── v3.0：参数化（对齐 DgcBrushSetting；存参供 B3-1 真实内核消费）── */

int dgcSetBrushSetting(DgcContext* ctx, DgcBrush brush, int settingId, double value) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (settingId < 0 || settingId >= DGC_SETTING_COUNT) {
        g_last_error = DGC_ERR_INVALID_ARG;
        return DGC_ERR_INVALID_ARG;
    }
    if (brush == DGC_INVALID_BRUSH) {
        g_last_error = DGC_ERR_INVALID_HANDLE;
        return DGC_ERR_INVALID_HANDLE;
    }
    if (settingId >= DGC_SETTING_WOBBLE_TIMEOUT_MS) {
        // D6-1：stroke modeler 参数（context 级单例，与具体笔刷句柄值无关，
        // 见 plan §4.4）。惰性激活（plan §4.2）：首次设置才创建 + 注入，之后
        // 仅 Configure 更新参数，dgcCreate 默认不注入，保证零回归。
        applyModelerSetting(ctx->impl_->model_params, settingId, value);
        if (ctx->impl_->predictor_handle_ == nullptr) {
            // 所有权：make_unique 创建 → engine->setPredictor 整体移交，
            // Engine::predictor_（unique_ptr）持有并负责析构；此处仅留一份
            // 非拥有裸指针供后续 Configure 调用，不 new/delete、不重复释放。
            auto predictor = std::make_unique<StrokeModeler>();
            StrokeModeler* raw = predictor.get();
            predictor->Configure(ctx->impl_->model_params);
            ctx->impl_->engine->setPredictor(std::move(predictor));
            ctx->impl_->predictor_handle_ = raw;
        } else {
            ctx->impl_->predictor_handle_->Configure(ctx->impl_->model_params);
        }
        g_last_error = DGC_OK;
        return DGC_OK;
    }
    ctx->impl_->brush_settings[brush][settingId] = value;
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcSetBrushColor(DgcContext* ctx, DgcBrush brush, float r, float g, float b, float a) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    if (brush == DGC_INVALID_BRUSH) {
        g_last_error = DGC_ERR_INVALID_HANDLE;
        return DGC_ERR_INVALID_HANDLE;
    }
    ctx->impl_->brush_colors[brush] = {r, g, b, a};
    // 桥接到内核（D6-3）：此前仅写 brush_colors（死存储，无读取点），改颜色不落地到
    // 渲染。补一跳转发到 IPaintKernel::setBrushColor（RGB→HSV→Brush::setColor），
    // 下一笔 strokeTo 的 dab 即用新色；旧笔迹已 composite 到画布，不受影响。
    // 约定：与 setBrushColor 本体同款并发 caveat——建议消费端在 stroke 前/引擎空闲时调用。
    ctx->impl_->kernel->setBrushColor(brush, r, g, b, a);
    g_last_error = DGC_OK;
    return DGC_OK;
}

/* ── v3.0：撤销（留接口不实现，撤销栈归后续任务）── */

int dgcUndo(DgcContext* ctx) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    g_last_error = DGC_ERR_NOT_IMPLEMENTED;
    return DGC_ERR_NOT_IMPLEMENTED;
}

/* ── v3.0：flush/drain 屏障（B5-2）── */

int dgcFlush(DgcContext* ctx) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    // 引擎未运行（dgcCreate 已 start；stop 后不再 running）时无需等待，直接返回。
    if (ctx->impl_->engine->running()) {
        ctx->impl_->engine->flush();
    }
    g_last_error = DGC_OK;
    return DGC_OK;
}

}  // extern "C"
