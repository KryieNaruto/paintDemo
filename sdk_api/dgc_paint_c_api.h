#ifndef DGC_PAINT_C_API_H
#define DGC_PAINT_C_API_H

/*
 * dgc_paint 唯一对外稳定 ABI（C 链接）。
 *
 * 消费者只 #include 本头，禁止 include core/ 等内部头。
 * 本头只依赖 <stdint.h>，不含任何 C++ 符号（无 class / template / std:: / 异常），
 * 可被纯 C 编译器直接解析；C++ 消费者经 extern "C" 守卫拿到 C 链接。
 *
 * 返回值约定：
 *   - 返回 int 的函数：0 = DGC_OK 成功，非 0 = 失败（具体见 DgcError）。
 *   - 返回句柄的函数：DGC_INVALID_BRUSH = 0 表示无效/失败。
 *   - dgcGetLastError()：有错误时返回描述串，无错误时返回 NULL（见下）。
 */

#include <stdint.h>

/* ── 符号导出宏（B5-1 可见性收紧 + Windows 支持）──
 * 共享库（Android .so / Windows DLL）配合 -fvisibility=hidden，只导出标记 DGC_API 的 C ABI 符号；
 * 头文件仍不暴露任何内部 C++ 类型。
 *   - Windows + DGCPAIN_SHARED：库构建者（DGC_PAINT_BUILDING）→ dllexport，消费者 → dllimport。
 *   - Windows 静态库（无 DGCPAIN_SHARED）：DGC_API 展开为空，无导入/导出。
 *   - GCC/Clang（Linux/Android）：visibility("default")（host 静态库下无害，默认即 default）。 */
#if defined(_WIN32)
#  if defined(DGCPAIN_SHARED) && defined(DGC_PAINT_BUILDING)
#    define DGC_API __declspec(dllexport)
#  elif defined(DGCPAIN_SHARED)
#    define DGC_API __declspec(dllimport)
#  else
#    define DGC_API
#  endif
#elif defined(__GNUC__) || defined(__clang__)
#  define DGC_API __attribute__((visibility("default")))
#else
#  define DGC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明上下文句柄：定义只在 .cpp 内，头不暴露任何内部 C++ 实现类型。 */
typedef struct DgcContext DgcContext;

/* 笔刷句柄：0 = DGC_INVALID_BRUSH（无效）。 */
typedef uint64_t DgcBrush;

typedef struct {
    float radius;
    float hardness;
    float opacity;
} DgcBrushParams;

#define DGC_INVALID_BRUSH ((DgcBrush)0)

/* 引擎固定默认笔刷句柄：Engine::start() 内同步创建的唯一笔刷（BrushKernel
 * next_handle 从 1 起，确定性地 = 1）。dgcCreateBrush 本期仍 DGC_ERR_NOT_IMPLEMENTED，
 * 消费端设色/设参数（dgcSetBrushColor / dgcSetBrushSetting）用该宏取默认笔刷句柄。 */
#define DGC_DEFAULT_BRUSH ((DgcBrush)1)

/* 错误码：int 返回函数 0 = 成功，非 0 = 失败；失败后可再 dgcGetLastError 读描述。 */
typedef enum {
    DGC_OK                 = 0, /* 成功 */
    DGC_ERR_NULL_CONTEXT   = 1, /* ctx 为 NULL */
    DGC_ERR_INVALID_ARG    = 2, /* 非法参数（负尺寸等） */
    DGC_ERR_INVALID_HANDLE = 3, /* 无效笔刷句柄 */
    DGC_ERR_NOT_IMPLEMENTED = 4, /* 本期未实现（myb 加载等） */
    DGC_ERR_QUEUE_FULL     = 5  /* 输入队列满（可重试/丢弃） */
} DgcError;

/* 创建/销毁上下文。失败返回 NULL（当前无失败路径）。 */
DGC_API DgcContext* dgcCreate(void);
DGC_API void        dgcDestroy(DgcContext* ctx);

/* 表面/尺寸。Null 后端下 nativeWindow 可为 NULL（headless）。 */
DGC_API int dgcSetSurface(DgcContext* ctx, void* nativeWindow, int w, int h);
DGC_API int dgcResize(DgcContext* ctx, int w, int h);

/* 笔画事件流。isPredicted 非 0 表示预测点。 */
DGC_API int dgcBeginStroke(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY);
DGC_API int dgcStrokeTo(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY, int isPredicted);
DGC_API int dgcEndStroke(DgcContext* ctx);

/* 笔刷管理。myb 加载本期未实现（无 L5），返回 DGC_INVALID_BRUSH + 错误码。 */
DGC_API DgcBrush dgcCreateBrush(DgcContext* ctx, const DgcBrushParams* params);
DGC_API DgcBrush dgcLoadBrushFromMyb(DgcContext* ctx, const char* mybJson);
DGC_API int      dgcDestroyBrush(DgcContext* ctx, DgcBrush brush);
DGC_API int      dgcSetBrush(DgcContext* ctx, DgcBrush brush);

/* 渲染/清屏。
 * dgcRender 仅驱动 SDK 离屏合成（render/vulkan/vk_backend.h：无 swapchain，
 * present() no-op），不涉及上屏/垂直同步（vsync）——vsync 归属见 README「垂直同步
 * （vsync）归属」小节：由消费端 present 模式（如 GLFW glfwSwapInterval）负责。 */
DGC_API int dgcRender(DgcContext* ctx);
DGC_API int dgcClear(DgcContext* ctx, float r, float g, float b, float a);

/* 最近一次错误描述（线程局部）：有错误返回静态描述串，无错误返回 NULL。 */
DGC_API const char* dgcGetLastError(void);

/* 笔刷参数化 settingId 常量（C API 层自定；真实内核语义归 B3-1）。 */
typedef enum {
    DGC_SETTING_RADIUS     = 0, /* 半径 */
    DGC_SETTING_HARDNESS   = 1, /* 硬度 */
    DGC_SETTING_OPACITY    = 2, /* 不透明度 */
    DGC_SETTING_RADIUS_LOG = 3, /* §4.0.6 radius_logarithmic 别名 */
    DGC_SETTING_COUNT      = 4  /* 非法值校验上界 */
} DgcBrushSetting;

/* ── v3.0：离屏 / 导出 / 像素读回（真实实现归 B2-1，本期 NOT_IMPLEMENTED）── */
DGC_API int dgcSetOffscreenSurface(DgcContext* ctx, int w, int h);
DGC_API int dgcExportPNG(DgcContext* ctx, const char* path);
DGC_API int dgcReadbackPixels(DgcContext* ctx, void* rgbaOut);

/* ── v3.0：确定性（B1-7 接线：注入/重播种 Mt19937Random + 固定时间步进）── */
DGC_API int dgcSetRandomSeed(DgcContext* ctx, uint64_t seed);
/* fixed_time_us 为固定时间步长（微秒）：override 后每点 t_us = n * fixed_time_us，
 * 每笔画 dgcBeginStroke 归零递增（首点 0、次点 step、再点 2*step …）；负值语义未定义，不校验。 */
DGC_API int dgcSetFixedTime(DgcContext* ctx, double t_us);

/* ── v3.0：参数化（对齐 DgcBrushSetting）── */
DGC_API int dgcSetBrushSetting(DgcContext* ctx, DgcBrush brush, int settingId, double value);
DGC_API int dgcSetBrushColor(DgcContext* ctx, DgcBrush brush, float r, float g, float b, float a);

/* ── v3.0：撤销（留接口不实现，撤销栈归后续任务）── */
DGC_API int dgcUndo(DgcContext* ctx);

/* ── v3.0：flush/drain 屏障（B5-2 第 23 函数，additive）──
 * 阻塞至所有已提交笔画输入已被渲染线程合成完毕，供离屏「入队 → export」的同步读回。
 * 引擎未运行时无需等待，直接返回 DGC_OK；ctx 为 NULL 返回 DGC_ERR_NULL_CONTEXT。 */
DGC_API int dgcFlush(DgcContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DGC_PAINT_C_API_H */
