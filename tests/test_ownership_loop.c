/* B1-8 所有权压力测试：循环 N 轮 dgcCreate → (可选 dgcSetOffscreenSurface) → dgcDestroy，
 * 证明 create/destroy 无泄漏、无崩溃、无 double-free / use-after-free（ASan/LSan 兜底）。
 *
 * 兼容 Null / Vulkan 两种后端：offscreen 返回码只做「合法（OK 或 NOT_IMPLEMENTED）」断言，
 * 不硬断言 OK（Null 后端 supportsOffscreen=false → NOT_IMPLEMENTED）。
 */
#include "dgc_paint_c_api.h"

#include <stdio.h>

#ifndef LOOP_ITERATIONS
#define LOOP_ITERATIONS 1000
#endif

int main(void) {
    for (int i = 0; i < LOOP_ITERATIONS; ++i) {
        DgcContext* ctx = dgcCreate();
        if (ctx == NULL) {
            fprintf(stderr, "FAIL: dgcCreate returned NULL at iteration %d\n", i);
            return 1;
        }
        /* 可选离屏：仅断言返回码合法（Vulkan 后端 OK，Null 后端 NOT_IMPLEMENTED）。 */
        int rc = dgcSetOffscreenSurface(ctx, 64, 64);
        if (rc != DGC_OK && rc != DGC_ERR_NOT_IMPLEMENTED) {
            fprintf(stderr, "FAIL: dgcSetOffscreenSurface rc=%d at iteration %d\n", rc, i);
            dgcDestroy(ctx);
            return 1;
        }
        dgcDestroy(ctx);
    }
    return 0;
}
