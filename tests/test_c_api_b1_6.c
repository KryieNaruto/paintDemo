/* B1-6 + B2-1 纯 C 冒烟：验证 v3.0 新增函数的返回码语义。
 *
 * B2-1 起离屏/导出/readback 由后端能力决定：
 *   - Vulkan 后端激活（supportsOffscreen=true）→ 返回 OK；
 *   - Null 后端（DGCPAIN_RENDER_VULKAN=OFF）→ 返回 NOT_IMPLEMENTED。
 * 本测试用运行时能力探测（先调 dgcSetOffscreenSurface 看返回）兼容两种配置。
 * 撤销仍 NOT_IMPLEMENTED；确定性存参返回 OK；参数化校验 handle/settingId。 */
#include "dgc_paint_c_api.h"

#include <stdio.h>
#include <string.h>

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

    /* 离屏 / 导出 / readback：按后端能力分支。 */
    int offscreenRc = dgcSetOffscreenSurface(ctx, 64, 64);
    if (offscreenRc == DGC_OK) {
        /* Vulkan 后端：三函数真实实现，返回 OK。 */
        unsigned char buf[64 * 64 * 4];
        memset(buf, 0, sizeof(buf));
        CHECK(dgcReadbackPixels(ctx, buf) == DGC_OK, "readback -> OK (vulkan)");
        CHECK(dgcExportPNG(ctx, "/tmp/dgc_b2_1_test.png") == DGC_OK,
              "exportPNG -> OK (vulkan)");
        /* 非法参数校验：null buffer → INVALID_ARG。 */
        CHECK(dgcReadbackPixels(ctx, NULL) == DGC_ERR_INVALID_ARG,
              "readback(NULL) -> INVALID_ARG");
    } else {
        /* Null 后端：三函数保持 NOT_IMPLEMENTED（合法参数）。 */
        unsigned char buf[1];
        CHECK(offscreenRc == DGC_ERR_NOT_IMPLEMENTED,
              "offscreen -> NOT_IMPLEMENTED (null backend)");
        CHECK(dgcExportPNG(ctx, "/tmp/out.png") == DGC_ERR_NOT_IMPLEMENTED,
              "exportPNG -> NOT_IMPLEMENTED (null backend)");
        CHECK(dgcReadbackPixels(ctx, buf) == DGC_ERR_NOT_IMPLEMENTED,
              "readback -> NOT_IMPLEMENTED (null backend)");
    }

    /* 负尺寸 / 空 path 非法参数校验（独立于后端能力）。 */
    CHECK(dgcSetOffscreenSurface(ctx, -1, 64) == DGC_ERR_INVALID_ARG,
          "offscreen(-1) -> INVALID_ARG");
    CHECK(dgcExportPNG(ctx, NULL) == DGC_ERR_INVALID_ARG, "exportPNG(NULL) -> INVALID_ARG");

    /* 撤销：仍 NOT_IMPLEMENTED（撤销栈归后续任务）。 */
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
    CHECK(dgcReadbackPixels(NULL, NULL) == DGC_ERR_NULL_CONTEXT,
          "NULL ctx readback -> NULL_CONTEXT");

    dgcDestroy(ctx);

    return failures == 0 ? 0 : 1;
}
