// bugfix 回归补充：模拟 paint-pc 真实 GUI 循环——笔画进行中，每个点后都 dgcReadbackPixels（无 flush）。
// 与既有回归测试 test_readback_drain（仅笔画结束后读回一次）互补，覆盖"边画边读"逐帧场景。
#include <cstdint>
#include <cstdio>
#include <vector>
#include "dgc_paint_c_api.h"

static int CountInk(const std::vector<std::uint8_t>& buf, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &buf[((size_t)y * w + x) * 4];
            if (!(p[0] > 200 && p[1] > 200 && p[2] > 200)) ++n;
        }
    return n;
}

int main() {
    constexpr int kW = 256, kH = 256;
    DgcContext* ctx = dgcCreate();
    dgcSetOffscreenSurface(ctx, kW, kH);
    dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f);

    std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 0);
    int minInkSeen = 999999, maxInkSeen = 0;
    int monotonicViolations = 0;
    int prevInk = 0;

    dgcBeginStroke(ctx, 5.0f, 128.0f, 1.0f, 0.f, 0.f);
    for (int x = 10; x <= 250; x += 2) {
        dgcStrokeTo(ctx, (float)x, 128.0f, 1.0f, 0.f, 0.f, 0);
        // 模拟真实 GUI：每个输入点后立刻读回（无 flush），与 app.cpp 主循环一致。
        dgcReadbackPixels(ctx, buf.data());
        int ink = CountInk(buf, kW, kH);
        if (ink < prevInk) ++monotonicViolations; // 墨迹应单调不减（drain 后不会"倒退"）
        prevInk = ink;
        if (ink < minInkSeen) minInkSeen = ink;
        if (ink > maxInkSeen) maxInkSeen = ink;
    }
    dgcEndStroke(ctx);

    // 最终 flush 后读回 = 权威完整画布
    dgcFlush(ctx);
    std::vector<std::uint8_t> finalBuf((size_t)kW * kH * 4, 0);
    dgcReadbackPixels(ctx, finalBuf.data());
    int finalInk = CountInk(finalBuf, kW, kH);

    std::fprintf(stderr,
        "[midstroke] frames=121 monotonic_violations=%d last_midstroke_ink=%d final_ink=%d 缺=%d (%.1f%%)\n",
        monotonicViolations, prevInk, finalInk, finalInk - prevInk,
        finalInk > 0 ? 100.0 * (finalInk - prevInk) / finalInk : 0.0);

    dgcDestroy(ctx);
    // 通过条件：过程中无"倒退"（单调不减，捕捉阻塞 drain / 丢 dab 的真实孔洞），且末次逐帧
    // 读回相对权威完整画布的滞后有界。P7-2 × Bug#3 修复把快照刷新节流到 ≤kMinFlushIntervalMs
    // （4ms），读回缓存因此允许「≤4ms + 渲染线程积压」的有界滞后（非永久孔洞——flush 后
    // finalInk 仍完整）；本用例是 121 点的小笔画、逐点读回，该有界滞后约占 10%~40%。故滞后
    // 阈值从原先假设「≈1 点滞后」的 5% 放宽到 50%，用于区分「有界滞后」与「阻塞型回归」
    // （历史 20fps 病理下末次读回接近 0% 墨迹、滞后 ≈100%）。单调不减断言不放松，仍可捕捉
    // 真实孔洞/倒退。
    bool pass = (finalInk - prevInk) < finalInk / 2 && monotonicViolations == 0;
    std::fprintf(stderr, pass ? "[midstroke] PASS\n" : "[midstroke] FAIL\n");
    return pass ? 0 : 1;
}
