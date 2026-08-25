/* B5-1 纯 C headless C API 闭环（§阶段1 test_c_api 落地）。
 *
 * 只 include "dgc_paint_c_api.h"，不 include 任何内部头；无窗口/无显示依赖，
 * 走完一整条 C API 会话：dgcCreate → dgcSetOffscreenSurface → dgcClear →
 * dgcBeginStroke/dgcStrokeTo×N/dgcEndStroke → dgcFlush → dgcReadbackPixels
 * （断言笔迹处相对底色变暗、笔迹外仍为底色）→ dgcExportPNG → dgcDestroy。
 *
 * 运行时能力探测（同 test_c_api_b1_6.c）：
 *   - Vulkan 后端激活（supportsOffscreen=true）→ 走真实 readback/export 像素断言；
 *   - Null 后端（DGCPAIN_RENDER_VULKAN=OFF）→ dgcSetOffscreenSurface 返回
 *     DGC_ERR_NOT_IMPLEMENTED，跳过像素断言但仍通过。 */
#include "dgc_paint_c_api.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, name)                       \
    do {                                        \
        if (!(cond)) {                          \
            fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                         \
        }                                       \
    } while (0)

int main(void) {
    DgcContext* ctx = dgcCreate();
    CHECK(ctx != NULL, "dgcCreate non-null");

    if (ctx != NULL) {
        int rc = dgcSetOffscreenSurface(ctx, 64, 64);
        if (rc == DGC_OK) {
            /* Vulkan 后端：真实 readback/export + 像素断言。 */
            CHECK(dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f) == DGC_OK, "clear OK");

            /* 水平直线 y=32，x 从 8 到 56（step 4px），默认笔刷。 */
            CHECK(dgcBeginStroke(ctx, 8.0f, 32.0f, 0.5f, 0.0f, 0.0f) == DGC_OK,
                  "beginStroke OK");
            for (int x = 12; x <= 56; x += 4) {
                CHECK(dgcStrokeTo(ctx, (float)x, 32.0f, 0.5f, 0.0f, 0.0f, 0) == DGC_OK,
                      "strokeTo OK");
            }
            CHECK(dgcEndStroke(ctx) == DGC_OK, "endStroke OK");
            CHECK(dgcFlush(ctx) == DGC_OK, "flush OK");

            unsigned char buf[64 * 64 * 4];
            memset(buf, 0, sizeof(buf));
            CHECK(dgcReadbackPixels(ctx, buf) == DGC_OK, "readback OK");

            const unsigned char* center = &buf[(32 * 64 + 32) * 4];  /* 笔迹覆盖处 */
            const unsigned char* corner = &buf[(0 * 64 + 0) * 4];    /* 笔迹之外 */
            fprintf(stderr,
                    "[test_c_api_headless] center=(%d,%d,%d) corner=(%d,%d,%d)\n",
                    center[0], center[1], center[2], corner[0], corner[1], corner[2]);

            CHECK(center[0] < 200, "stroke darkened center (vs white bg)");
            CHECK(corner[0] > 200 && corner[1] > 200 && corner[2] > 200,
                  "corner stays white");
            CHECK(dgcExportPNG(ctx, "/tmp/b5_1_headless.png") == DGC_OK, "exportPNG OK");
        } else {
            /* Null 后端：离屏三函数回 NOT_IMPLEMENTED，跳过像素断言。 */
            CHECK(rc == DGC_ERR_NOT_IMPLEMENTED,
                  "offscreen -> NOT_IMPLEMENTED (null backend)");
        }

        dgcDestroy(ctx);
    }

    if (failures == 0) {
        fprintf(stderr, "[test_c_api_headless] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
