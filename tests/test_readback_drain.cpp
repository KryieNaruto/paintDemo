// bugfix repro：dgcReadbackPixels 不先 drain 时可能返回不完整画布。
//
// 模拟 paint-pc 消费端行为：喂入大量笔画点后【不调 dgcFlush】立即 dgcReadbackPixels。
// 三线程引擎是异步的，渲染线程合成有滞后；若 readback 不先 drain，拷贝到的画布
// 可能缺最近提交但尚未合成的 dab → 线条出现空洞。
//
// 回归断言：无 flush 直接读回的墨迹数 应 ≈ flush 后读回的墨迹数（完整画布）。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dgc_paint_c_api.h"

static int failures = 0;
#define CHECK(cond, name)                       \
    do {                                        \
        if (!(cond)) {                          \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                         \
        }                                       \
    } while (0)

static int CountInk(const std::vector<std::uint8_t>& buf, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &buf[((size_t)y * w + x) * 4];
            // 白底 (255,255,255)：非白 = 墨迹。
            if (!(p[0] > 200 && p[1] > 200 && p[2] > 200)) {
                ++n;
            }
        }
    }
    return n;
}

int main() {
    constexpr int kW = 256;
    constexpr int kH = 256;

    DgcContext* ctx = dgcCreate();
    CHECK(ctx != nullptr, "dgcCreate non-null");
    if (ctx == nullptr) return 1;

    CHECK(dgcSetOffscreenSurface(ctx, kW, kH) == DGC_OK, "offscreen init OK");
    CHECK(dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f) == DGC_OK, "clear white");

    // 一条长直线：大量点喂入，触发渲染线程滞后。
    dgcBeginStroke(ctx, 5.0f, 128.0f, 1.0f, 0.f, 0.f);
    for (int x = 10; x <= 250; x += 2) {
        dgcStrokeTo(ctx, (float)x, 128.0f, 1.0f, 0.f, 0.f, 0);
    }
    dgcEndStroke(ctx);

    // 模拟 paint-pc：不调 dgcFlush，直接读回。
    std::vector<std::uint8_t> buf1((size_t)kW * kH * 4, 0);
    CHECK(dgcReadbackPixels(ctx, buf1.data()) == DGC_OK, "readback no-flush OK");
    const int inkNoFlush = CountInk(buf1, kW, kH);

    // 对照：flush（drain 屏障）后读回 = 完整画布。
    CHECK(dgcFlush(ctx) == DGC_OK, "flush OK");
    std::vector<std::uint8_t> buf2((size_t)kW * kH * 4, 0);
    CHECK(dgcReadbackPixels(ctx, buf2.data()) == DGC_OK, "readback after-flush OK");
    const int inkFlushed = CountInk(buf2, kW, kH);

    std::fprintf(stderr,
                 "[test_readback_drain] ink no-flush=%d flush=%d 缺=%d (%.1f%%)\n",
                 inkNoFlush, inkFlushed, inkFlushed - inkNoFlush,
                 inkFlushed > 0 ? 100.0 * (inkFlushed - inkNoFlush) / inkFlushed : 0.0);

    // 回归断言：无 flush 读回应返回完整画布（缺 < 5%）。
    CHECK(inkFlushed > 0, "flushed canvas has ink");
    const int missing = inkFlushed - inkNoFlush;
    CHECK(missing < inkFlushed / 20, "readback without flush is complete (missing<5%)");

    dgcDestroy(ctx);

    if (failures == 0) {
        std::fprintf(stderr, "[test_readback_drain] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
