/* 纯 C 编译冒烟：验证 sdk_api/dgc_paint_c_api.h 可被 C 编译器解析，
 * 且实现已链接进 dgc_paint（取函数地址不报未定义符号）。 */
#include "dgc_paint_c_api.h"

int main(void) {
    /* 取全部 22 个公开函数地址，证明声明齐全且链接进库。 */
    DgcContext* (*p_create)(void) = dgcCreate;
    void (*p_destroy)(DgcContext*) = dgcDestroy;
    int (*p_set_surface)(DgcContext*, void*, int, int) = dgcSetSurface;
    int (*p_resize)(DgcContext*, int, int) = dgcResize;
    int (*p_begin)(DgcContext*, float, float, float, float, float) = dgcBeginStroke;
    int (*p_stroke_to)(DgcContext*, float, float, float, float, float, int) = dgcStrokeTo;
    int (*p_end)(DgcContext*) = dgcEndStroke;
    DgcBrush (*p_create_brush)(DgcContext*, const DgcBrushParams*) = dgcCreateBrush;
    DgcBrush (*p_load_myb)(DgcContext*, const char*) = dgcLoadBrushFromMyb;
    int (*p_destroy_brush)(DgcContext*, DgcBrush) = dgcDestroyBrush;
    int (*p_set_brush)(DgcContext*, DgcBrush) = dgcSetBrush;
    int (*p_render)(DgcContext*) = dgcRender;
    int (*p_clear)(DgcContext*, float, float, float, float) = dgcClear;
    const char* (*p_last_error)(void) = dgcGetLastError;
    /* v3.0（B1-6）新增 8 个。 */
    int (*p_set_offscreen)(DgcContext*, int, int) = dgcSetOffscreenSurface;
    int (*p_export_png)(DgcContext*, const char*) = dgcExportPNG;
    int (*p_readback)(DgcContext*, void*) = dgcReadbackPixels;
    int (*p_set_seed)(DgcContext*, uint64_t) = dgcSetRandomSeed;
    int (*p_set_fixed_time)(DgcContext*, double) = dgcSetFixedTime;
    int (*p_set_brush_setting)(DgcContext*, DgcBrush, int, double) = dgcSetBrushSetting;
    int (*p_set_brush_color)(DgcContext*, DgcBrush, float, float, float, float) = dgcSetBrushColor;
    int (*p_undo)(DgcContext*) = dgcUndo;

    /* 保持符号被引用，避免未使用告警（-Werror 下也安全）。 */
    (void)p_create;
    (void)p_destroy;
    (void)p_set_surface;
    (void)p_resize;
    (void)p_begin;
    (void)p_stroke_to;
    (void)p_end;
    (void)p_create_brush;
    (void)p_load_myb;
    (void)p_destroy_brush;
    (void)p_set_brush;
    (void)p_render;
    (void)p_clear;
    (void)p_last_error;
    (void)p_set_offscreen;
    (void)p_export_png;
    (void)p_readback;
    (void)p_set_seed;
    (void)p_set_fixed_time;
    (void)p_set_brush_setting;
    (void)p_set_brush_color;
    (void)p_undo;

    return 0;
}
