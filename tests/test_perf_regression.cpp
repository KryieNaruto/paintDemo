// bugfix 回归：dgcReadbackPixels 不应阻塞在渲染线程的 composite 上。
//
// d64c2fa 把 engine->flush() 塞进了 dgcReadbackPixels（修复空洞），但代价是 GUI 主循环
// 每帧读回都要同步等渲染线程把当前积压的 dab composite 完——快速甩笔时一批要合成几十个
// dab，直接体现成掉帧（PC 20fps 回退）。本用例模拟真实 paint-pc 主循环：快速甩笔（每帧
// 40~80px 位移，覆盖 1920x1080 大画布）+ 每帧读回，断言不会有任何单帧超过 20ms
// （50fps 下限，比用户反馈的 20fps 门槛留足安全边际）。
//
// 正确的修复（快照缓存）应让 dgcReadbackPixels 退化成一次内存拷贝，不含 GPU 提交/等待，
// 不随渲染线程的 composite 批大小波动。
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dgc_paint_c_api.h"

static int failures = 0;
#define CHECK(cond, name)                            \
    do {                                              \
        if (!(cond)) {                                \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                                \
        }                                              \
    } while (0)

int main() {
    constexpr int kW = 1920, kH = 1080;
    DgcContext* ctx = dgcCreate();
    CHECK(ctx != nullptr, "dgcCreate non-null");
    if (ctx == nullptr) return 1;

    CHECK(dgcSetOffscreenSurface(ctx, kW, kH) == DGC_OK, "offscreen init OK");
    CHECK(dgcClear(ctx, 0.96f, 0.95f, 0.91f, 1.0f) == DGC_OK, "clear OK");

    std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 0);

    // 快速甩笔：每帧鼠标位移 40~80px（真实用户快速挥动手腕的典型帧间距离），
    // 每点后都读回一次——真实 paint-pc 主循环每帧必做的调用序列。
    dgcBeginStroke(ctx, 100.f, 100.f, 0.5f, 0.f, 0.f);
    std::vector<double> frameMs;
    frameMs.reserve(300);
    float x = 100.f, y = 400.f;
    for (int i = 0; i < 300; ++i) {
        x += 40.f + 40.f * std::fabs(std::sin((float)i * 0.13f));  // 40~80px/帧
        y = 400.f + 200.f * std::sin((float)i * 0.07f);
        if (x > 1850.f) x = 100.f;
        auto t0 = std::chrono::steady_clock::now();
        dgcStrokeTo(ctx, x, y, 0.5f, 0.f, 0.f, 0);
        dgcReadbackPixels(ctx, buf.data());
        auto t1 = std::chrono::steady_clock::now();
        frameMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
    }
    dgcEndStroke(ctx);
    dgcDestroy(ctx);

    double sum = 0, mx = 0;
    for (double v : frameMs) {
        sum += v;
        if (v > mx) mx = v;
    }
    const double avg = sum / frameMs.size();
    std::vector<double> sorted = frameMs;
    std::sort(sorted.begin(), sorted.end());
    const double p95 = sorted[(size_t)(sorted.size() * 0.95)];

    int under30fps = 0, under20fps = 0;
    for (double v : frameMs) {
        if (v > 1000.0 / 30.0) ++under30fps;
        if (v > 1000.0 / 20.0) ++under20fps;
    }

    std::fprintf(stderr,
                 "[test_perf_regression] frames=%zu avg=%.3fms p95=%.3fms max=%.3fms "
                 "avgFPS=%.1f worstFPS=%.1f under30fps=%d/%zu under20fps=%d/%zu\n",
                 frameMs.size(), avg, p95, mx, 1000.0 / avg, 1000.0 / mx, under30fps,
                 frameMs.size(), under20fps, frameMs.size());

    // 核心断言：readback 是纯 memcpy 快照读取，不应受 composite 批大小影响——
    // 50fps 下限（20ms/帧），比用户反馈的 20fps 门槛留足安全边际。
    CHECK(mx < 20.0, "no single frame exceeds 20ms under fast-stroke load");

    if (failures == 0) {
        std::fprintf(stderr, "[test_perf_regression] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
