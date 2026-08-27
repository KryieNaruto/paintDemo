/* B1-6 + B2-1 + D6-1 纯 C 冒烟：验证 v3.0 新增函数的返回码语义。
 *
 * B2-1 起离屏/导出/readback 由后端能力决定：
 *   - Vulkan 后端激活（supportsOffscreen=true）→ 返回 OK；
 *   - Null 后端（DGCPAIN_RENDER_VULKAN=OFF）→ 返回 NOT_IMPLEMENTED。
 * 本测试用运行时能力探测（先调 dgcSetOffscreenSurface 看返回）兼容两种配置。
 * 撤销仍 NOT_IMPLEMENTED；确定性存参返回 OK；参数化校验 handle/settingId。
 *
 * D6-1 新增：dgcCreateBrush 返回有效句柄；9 个 stroke modeler settingId
 * （DGC_SETTING_WOBBLE_TIMEOUT_MS .. DGC_SETTING_PREDICTION_INTERVAL_MS）返回
 * DGC_OK；越界/负 settingId 返回 DGC_ERR_INVALID_ARG 且 dgcGetLastError 非 NULL；
 * 离屏可用时用「改 modeler 参数后渲染像素与默认不同」证明真实透传到
 * stroke predictor（而非只是存参）。 */
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

    /* D6-1：dgcCreateBrush 返回有效（非 0）发号器句柄。 */
    DgcBrush brush = dgcCreateBrush(ctx, NULL);
    CHECK(brush != DGC_INVALID_BRUSH, "createBrush -> valid handle");

    /* D6-1：9 个 stroke modeler settingId 均返回 DGC_OK（透传到 core/stroke_predictor.h）。 */
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_WOBBLE_TIMEOUT_MS, 40.0) == DGC_OK,
          "setBrushSetting(wobble_timeout_ms) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_WOBBLE_SPEED_FLOOR, 1.31) == DGC_OK,
          "setBrushSetting(wobble_speed_floor) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_MIN_OUTPUT_RATE_HZ, 180.0) == DGC_OK,
          "setBrushSetting(min_output_rate_hz) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_END_OF_STROKE_STOPPING_DISTANCE_MM, 0.1) ==
              DGC_OK,
          "setBrushSetting(end_of_stroke_stopping_distance_mm) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_SPRING_MASS_CONSTANT, 400.0) == DGC_OK,
          "setBrushSetting(spring_mass_constant) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_SPRING_DRAG_CONSTANT, 40.0) == DGC_OK,
          "setBrushSetting(spring_drag_constant) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_KALMAN_PROCESS_NOISE, 0.0005) == DGC_OK,
          "setBrushSetting(kalman_process_noise) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_KALMAN_MEASUREMENT_NOISE, 0.004) == DGC_OK,
          "setBrushSetting(kalman_measurement_noise) -> OK");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_PREDICTION_INTERVAL_MS, 16.0) == DGC_OK,
          "setBrushSetting(prediction_interval_ms) -> OK");

    /* D6-1：越界/负 settingId 仍返回 INVALID_ARG，且 dgcGetLastError 非 NULL（不崩溃）。 */
    CHECK(dgcSetBrushSetting(ctx, brush, -1, 0.0) == DGC_ERR_INVALID_ARG,
          "setBrushSetting(-1) -> INVALID_ARG");
    CHECK(dgcGetLastError() != NULL, "getLastError() non-NULL after invalid settingId(-1)");
    CHECK(dgcSetBrushSetting(ctx, brush, DGC_SETTING_COUNT, 0.0) == DGC_ERR_INVALID_ARG,
          "setBrushSetting(DGC_SETTING_COUNT) -> INVALID_ARG");
    CHECK(dgcGetLastError() != NULL,
          "getLastError() non-NULL after invalid settingId(COUNT)");

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

    /* D6-1：离屏可用时，验证「设 modeler 参数后渲染像素与默认渲染不同」，证明真实
     * 透传到 stroke predictor（惰性激活生效），而不仅是存参不作用。用两个独立
     * context 跑相同笔画序列：A 保持默认（未碰 modeler settingId，passthrough），
     * B 触发一次 modeler settingId（即便仍是默认数值，激活本身即改变管线），
     * 二者读回像素应不同。 */
    {
        DgcContext* ctxA = dgcCreate();
        DgcContext* ctxB = dgcCreate();
        int offA = dgcSetOffscreenSurface(ctxA, 64, 64);
        int offB = dgcSetOffscreenSurface(ctxB, 64, 64);
        if (offA == DGC_OK && offB == DGC_OK) {
            dgcClear(ctxA, 1.0f, 1.0f, 1.0f, 1.0f);
            dgcClear(ctxB, 1.0f, 1.0f, 1.0f, 1.0f);

            DgcBrush brushA = dgcCreateBrush(ctxA, NULL);
            DgcBrush brushB = dgcCreateBrush(ctxB, NULL);
            CHECK(brushA != DGC_INVALID_BRUSH, "createBrush -> valid handle (A)");
            CHECK(brushB != DGC_INVALID_BRUSH, "createBrush -> valid handle (B)");

            /* 只在 B 上激活 modeler（预测间隔调大，预测点更远，笔迹应可辨差异）。 */
            CHECK(dgcSetBrushSetting(ctxB, brushB, DGC_SETTING_PREDICTION_INTERVAL_MS, 200.0) ==
                      DGC_OK,
                  "setBrushSetting(prediction_interval_ms, large) -> OK");

            /* 相同笔画序列，喂给 A（passthrough）与 B（modeler 激活）。 */
            dgcBeginStroke(ctxA, 10.0f, 10.0f, 1.0f, 0.0f, 0.0f);
            dgcBeginStroke(ctxB, 10.0f, 10.0f, 1.0f, 0.0f, 0.0f);
            for (int i = 1; i <= 20; ++i) {
                float t = (float)i;
                dgcStrokeTo(ctxA, 10.0f + t, 10.0f + t, 1.0f, 0.0f, 0.0f, 0);
                dgcStrokeTo(ctxB, 10.0f + t, 10.0f + t, 1.0f, 0.0f, 0.0f, 0);
            }
            dgcEndStroke(ctxA);
            dgcEndStroke(ctxB);
            dgcFlush(ctxA);
            dgcFlush(ctxB);

            unsigned char bufA[64 * 64 * 4];
            unsigned char bufB[64 * 64 * 4];
            CHECK(dgcReadbackPixels(ctxA, bufA) == DGC_OK, "readback A -> OK");
            CHECK(dgcReadbackPixels(ctxB, bufB) == DGC_OK, "readback B -> OK");
            CHECK(memcmp(bufA, bufB, sizeof(bufA)) != 0,
                  "modeler settingId 激活后渲染像素与默认渲染不同（透传真实生效）");
        }
        dgcDestroy(ctxA);
        dgcDestroy(ctxB);
    }

    return failures == 0 ? 0 : 1;
}
