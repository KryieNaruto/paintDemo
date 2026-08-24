/* B1-6 纯 C 冒烟：验证 v3.0 新增 8 个函数的返回码语义。
 * 本期口径：离屏/导出/readback/撤销为 NOT_IMPLEMENTED，确定性存参返回 OK，
 * 参数化校验 handle/settingId。真实内核归 B1-7/B2-1/B3-1。 */
#include "dgc_paint_c_api.h"

#include <stdio.h>

static int failures = 0;

#define CHECK(cond, name)                     \
    do {                                      \
        if (!(cond)) {                        \
            fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                       \
        }                                     \
    } while (0)

int main(void) {
    DgcContext* ctx = dgcCreate();

    /* 离屏 / 导出 / readback / 撤销：本期仅声明 + NOT_IMPLEMENTED。 */
    CHECK(dgcSetOffscreenSurface(ctx, 64, 64) == DGC_ERR_NOT_IMPLEMENTED,
          "offscreen -> NOT_IMPLEMENTED");
    CHECK(dgcExportPNG(ctx, "/tmp/out.png") == DGC_ERR_NOT_IMPLEMENTED,
          "exportPNG -> NOT_IMPLEMENTED");
    CHECK(dgcReadbackPixels(ctx, NULL) == DGC_ERR_NOT_IMPLEMENTED,
          "readback -> NOT_IMPLEMENTED");
    CHECK(dgcUndo(ctx) == DGC_ERR_NOT_IMPLEMENTED, "undo -> NOT_IMPLEMENTED");

    /* 确定性：存参返回 OK。 */
    CHECK(dgcSetRandomSeed(ctx, 42) == DGC_OK, "setRandomSeed -> OK");
    CHECK(dgcSetFixedTime(ctx, 1000000.0) == DGC_OK, "setFixedTime -> OK");

    /* 参数化：校验 handle / settingId（DGC_INVALID_BRUSH 无有效句柄，见 B1-4 遗留）。 */
    CHECK(dgcSetBrushSetting(ctx, DGC_INVALID_BRUSH, DGC_SETTING_RADIUS, 0.9)
              == DGC_ERR_INVALID_HANDLE,
          "setBrushSetting(valid setting) -> INVALID_HANDLE");
    CHECK(dgcSetBrushColor(ctx, DGC_INVALID_BRUSH, 0.0f, 0.0f, 0.0f, 1.0f)
              == DGC_ERR_INVALID_HANDLE,
          "setBrushColor -> INVALID_HANDLE");
    CHECK(dgcSetBrushSetting(ctx, DGC_INVALID_BRUSH, 999, 0.9) == DGC_ERR_INVALID_ARG,
          "setBrushSetting(bad settingId) -> INVALID_ARG");

    /* 空上下文：一律 NULL_CONTEXT。 */
    CHECK(dgcSetRandomSeed(NULL, 42) == DGC_ERR_NULL_CONTEXT,
          "NULL ctx setRandomSeed -> NULL_CONTEXT");
    CHECK(dgcSetBrushSetting(NULL, DGC_INVALID_BRUSH, DGC_SETTING_RADIUS, 0.9)
              == DGC_ERR_NULL_CONTEXT,
          "NULL ctx setBrushSetting -> NULL_CONTEXT");
    CHECK(dgcSetOffscreenSurface(NULL, 64, 64) == DGC_ERR_NULL_CONTEXT,
          "NULL ctx offscreen -> NULL_CONTEXT");

    dgcDestroy(ctx);

    return failures == 0 ? 0 : 1;
}
