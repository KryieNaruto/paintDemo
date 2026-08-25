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
DgcContext* dgcCreate(void);
void        dgcDestroy(DgcContext* ctx);

/* 表面/尺寸。Null 后端下 nativeWindow 可为 NULL（headless）。 */
int dgcSetSurface(DgcContext* ctx, void* nativeWindow, int w, int h);
int dgcResize(DgcContext* ctx, int w, int h);

/* 笔画事件流。isPredicted 非 0 表示预测点。 */
int dgcBeginStroke(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY);
int dgcStrokeTo(DgcContext* ctx, float x, float y, float pressure, float tiltX, float tiltY, int isPredicted);
int dgcEndStroke(DgcContext* ctx);

/* 笔刷管理。myb 加载本期未实现（无 L5），返回 DGC_INVALID_BRUSH + 错误码。 */
DgcBrush dgcCreateBrush(DgcContext* ctx, const DgcBrushParams* params);
DgcBrush dgcLoadBrushFromMyb(DgcContext* ctx, const char* mybJson);
int      dgcDestroyBrush(DgcContext* ctx, DgcBrush brush);
int      dgcSetBrush(DgcContext* ctx, DgcBrush brush);

/* 渲染/清屏。 */
int dgcRender(DgcContext* ctx);
int dgcClear(DgcContext* ctx, float r, float g, float b, float a);

/* 最近一次错误描述（线程局部）：有错误返回静态描述串，无错误返回 NULL。 */
const char* dgcGetLastError(void);

/* 笔刷参数化 settingId 常量（C API 层自定；真实内核语义归 B3-1）。 */
typedef enum {
    DGC_SETTING_RADIUS     = 0, /* 半径 */
    DGC_SETTING_HARDNESS   = 1, /* 硬度 */
    DGC_SETTING_OPACITY    = 2, /* 不透明度 */
    DGC_SETTING_RADIUS_LOG = 3, /* §4.0.6 radius_logarithmic 别名 */
    DGC_SETTING_COUNT      = 4  /* 非法值校验上界 */
} DgcBrushSetting;

/* ── v3.0：离屏 / 导出 / 像素读回（真实实现归 B2-1，本期 NOT_IMPLEMENTED）── */
int dgcSetOffscreenSurface(DgcContext* ctx, int w, int h);
int dgcExportPNG(DgcContext* ctx, const char* path);
int dgcReadbackPixels(DgcContext* ctx, void* rgbaOut);

/* ── v3.0：确定性（B1-7 接线：注入/重播种 Mt19937Random + 固定时间步进）── */
int dgcSetRandomSeed(DgcContext* ctx, uint64_t seed);
/* fixed_time_us 为固定时间步长（微秒）：override 后每点 t_us = n * fixed_time_us，
 * 每笔画 dgcBeginStroke 归零递增（首点 0、次点 step、再点 2*step …）；负值语义未定义，不校验。 */
int dgcSetFixedTime(DgcContext* ctx, double t_us);

/* ── v3.0：参数化（对齐 DgcBrushSetting）── */
int dgcSetBrushSetting(DgcContext* ctx, DgcBrush brush, int settingId, double value);
int dgcSetBrushColor(DgcContext* ctx, DgcBrush brush, float r, float g, float b, float a);

/* ── v3.0：撤销（留接口不实现，撤销栈归后续任务）── */
int dgcUndo(DgcContext* ctx);

/* ── v3.0：flush/drain 屏障（B5-2 第 23 函数，additive）──
 * 阻塞至所有已提交笔画输入已被渲染线程合成完毕，供离屏「入队 → export」的同步读回。
 * 引擎未运行时无需等待，直接返回 DGC_OK；ctx 为 NULL 返回 DGC_ERR_NULL_CONTEXT。 */
int dgcFlush(DgcContext* ctx);

#ifdef __cplusplus
}
#endif

#endif /* DGC_PAINT_C_API_H */
