#include "sdk_api/dgc_paint_c_api.h"

#include <array>
#include <memory>
#include <unordered_map>

#include "core/determinism.h"
#include "core/engine.h"
#include "core/interfaces/i_paint_kernel.h"
#include "core/interfaces/i_render_backend.h"
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
};

// 析构器必须在 Impl 完整后定义（unique_ptr<Impl> + 不完整类型，否则隐式析构在声明处
// 实例化会因 incomplete type 编译失败）。
DgcContext::~DgcContext() = default;

namespace {

// 错误码记录：dgcGetLastError 无 ctx 参数，故用线程局部变量。
thread_local DgcError g_last_error = DGC_OK;

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
    // 引擎固定默认笔刷，本期不接线按笔画选笔刷（见 B1-4 计划「风险」）。
    g_last_error = DGC_ERR_NOT_IMPLEMENTED;
    return DGC_INVALID_BRUSH;
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
    // 读回前先 drain：引擎为「输入→笔刷→渲染」三线程异步模型，渲染线程合成 dab
    // 有滞后；若直接 readback 拷贝画布，会缺最近提交但尚未合成的 dab（线条空洞）。
    // 与 dgcFlush 同逻辑，保证拷贝到的是已合成全部已提交输入的完整画布。
    if (ctx->impl_->engine->running()) {
        ctx->impl_->engine->flush();
    }
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
