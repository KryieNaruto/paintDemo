#include "sdk_api/dgc_paint_c_api.h"

#include <array>
#include <memory>
#include <unordered_map>

#include "core/engine.h"
#include "core/interfaces/i_paint_kernel.h"
#include "core/interfaces/i_render_backend.h"
#include "core/null/null_paint_kernel.h"
#include "core/types.h"
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
    // B1-6 新增：确定性 + 参数化存参（真实内核/渲染机制归 B1-7/B2-1/B3-1）。
    uint64_t random_seed    = 0;
    double   fixed_time_us  = 0.0;
    bool     fixed_time_set = false;  // 对齐 §4.0.3 override_time
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
    impl->kernel  = std::make_unique<NullPaintKernel>();
    impl->backend = CreateDefaultRenderBackend();  // unique_ptr<IRenderBackend>
    impl->engine  = std::make_unique<Engine>(impl->kernel.get(), impl->backend.get());
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
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcStrokeTo(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY, int isPredicted) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    StrokePoint p{x, y, pressure, tiltX, tiltY, 0, (isPredicted != 0)};
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
    (void)r;
    (void)g;
    (void)b;
    (void)a;
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    ctx->impl_->backend->clearCanvas();
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
    ctx->impl_->backend->readback(rgbaOut);
    g_last_error = DGC_OK;
    return DGC_OK;
}

/* ── v3.0：确定性（本期仅存参；ReplayRandom/固定步进内核归 B1-7）── */

int dgcSetRandomSeed(DgcContext* ctx, uint64_t seed) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    ctx->impl_->random_seed = seed;
    g_last_error = DGC_OK;
    return DGC_OK;
}

int dgcSetFixedTime(DgcContext* ctx, double t_us) {
    if (ctx == nullptr) {
        g_last_error = DGC_ERR_NULL_CONTEXT;
        return DGC_ERR_NULL_CONTEXT;
    }
    ctx->impl_->fixed_time_us  = t_us;
    ctx->impl_->fixed_time_set = true;
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

}  // extern "C"
