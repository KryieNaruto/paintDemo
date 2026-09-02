// P7-2 黑盒回归测试（C API 级）：字面覆盖"高频调用 dgcReadbackPixels"这一真实消费者
// 用法（PC 关 vsync 后 paint-pc 主循环的忙循环调用），互补 test_flush_throttle_engine.cpp
// 的白盒量化用例。
//
// 与既有 test_continuous_input_regression.cpp 的关键差异：后者读回循环内每次都调用
// CountInk（逐像素扫描 512×512），本身构成天然节流，达不到"最高频率"；本用例把
// CountInk 挪到固定间隔（每 200 次迭代）才采样一次，绝大多数迭代只做
// dgcReadbackPixels 本身，尽可能逼近"调用频率远超其影响所能达到的最高有效频率"这一
// 场景，验证 P7-2 新增的节流没有让批量 composite 打回逐条处理，也没有重新引入孔洞。
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "dgc_paint_c_api.h"

// 与 test_continuous_input_regression.cpp 同款 ASan 放宽口径（见该文件顶部注释）：
// 插桩下引擎整体吞吐可下降一个数量级以上，producer 的 sleep_for 是真实墙钟时间、
// 不受插桩影响，两者不对称会使消费者相对生产者更滞后。
#if defined(__SANITIZE_ADDRESS__)
#define DGC_ASAN_BUILD 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define DGC_ASAN_BUILD 1
#endif
#endif
#ifndef DGC_ASAN_BUILD
#define DGC_ASAN_BUILD 0
#endif

#if DGC_ASAN_BUILD
constexpr double kMaxCallMs = 150.0;  // 非 ASan 下 20ms；插桩下放宽约 7.5 倍。
constexpr int kMissingPercent = 50;   // 非 ASan 下 15%；插桩下放宽约 3.3 倍。
#else
constexpr double kMaxCallMs = 20.0;
constexpr int kMissingPercent = 15;
#endif

static int failures = 0;
#define CHECK(cond, name)                            \
    do {                                              \
        if (!(cond)) {                                \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                                \
        }                                              \
    } while (0)

static int CountInk(const std::vector<std::uint8_t>& buf, int w, int h) {
    int n = 0;
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            const std::uint8_t* p = &buf[((size_t)y * w + x) * 4];
            if (!(p[0] > 200 && p[1] > 200 && p[2] > 200)) {
                ++n;
            }
        }
    }
    return n;
}

int main() {
    constexpr int kW = 512;
    constexpr int kH = 512;
    constexpr int kPoints = 700;  // 与 test_continuous_input_regression.cpp 同量级路径。
    constexpr int kSampleEvery = 200;  // 绝大多数迭代只做 dgcReadbackPixels 本身。

    DgcContext* ctx = dgcCreate();
    CHECK(ctx != nullptr, "dgcCreate non-null");
    if (ctx == nullptr) return 1;

    CHECK(dgcSetOffscreenSurface(ctx, kW, kH) == DGC_OK, "offscreen init OK");
    CHECK(dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f) == DGC_OK, "clear white");

    std::atomic<bool> producerDone{false};

    std::thread producer([&]() {
        const float cx = kW / 2.0f, cy = kH / 2.0f;
        dgcBeginStroke(ctx, cx, cy, 0.6f, 0.f, 0.f);
        for (int i = 0; i < kPoints; ++i) {
            const float t = (float)i / kPoints;
            const float r = t * (kW / 2.2f);
            const float theta = t * 12.0f * 3.14159265f;
            const float x = cx + r * std::cos(theta);
            const float y = cy + r * std::sin(theta);
            dgcStrokeTo(ctx, x, y, 0.6f, 0.f, 0.f, 0);
            std::this_thread::sleep_for(std::chrono::microseconds(200 + (i % 4) * 100));
        }
        dgcEndStroke(ctx);
        producerDone.store(true, std::memory_order_release);
    });

    // 前台线程（模拟 paint-pc 关 vsync 后的忙循环）：紧凑循环调用 dgcReadbackPixels，
    // 不加任何 sleep，仅每 kSampleEvery 次才逐像素扫描一次（CountInk 采样，供孔洞
    // 检查用），最大化实际达到的调用频率。
    std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 0);
    std::vector<double> callMs;
    std::vector<int> inkSamples;
    callMs.reserve(4000);
    inkSamples.reserve(64);
    long calls = 0;
    int lastSample = 0;
    const auto loopStart = std::chrono::steady_clock::now();
    while (!producerDone.load(std::memory_order_acquire)) {
        auto t0 = std::chrono::steady_clock::now();
        CHECK(dgcReadbackPixels(ctx, buf.data()) == DGC_OK, "readback during hammering OK");
        auto t1 = std::chrono::steady_clock::now();
        callMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        ++calls;
        if (calls % kSampleEvery == 0) {
            lastSample = CountInk(buf, kW, kH);
            inkSamples.push_back(lastSample);
        }
    }
    const auto loopEnd = std::chrono::steady_clock::now();
    producer.join();  // RAII：显式 join，不 detach。

    // 收尾后再采一次样，覆盖 producer 刚结束、消费者还未来得及追上的窗口。
    lastSample = CountInk(buf, kW, kH);
    inkSamples.push_back(lastSample);

    // 权威对照：dgcFlush + 读回 = 完整画布。
    CHECK(dgcFlush(ctx) == DGC_OK, "flush OK");
    std::vector<std::uint8_t> bufFlushed((size_t)kW * kH * 4, 0);
    CHECK(dgcReadbackPixels(ctx, bufFlushed.data()) == DGC_OK, "readback after flush OK");
    const int inkFlushed = CountInk(bufFlushed, kW, kH);

    const double loopElapsedSec =
        std::chrono::duration<double>(loopEnd - loopStart).count();
    const double callsPerSec = loopElapsedSec > 0 ? calls / loopElapsedSec : 0.0;

    double maxMs = 0;
    for (double v : callMs) {
        if (v > maxMs) maxMs = v;
    }
    std::fprintf(stderr,
                 "[test_flush_throttle_readback] calls=%ld elapsedSec=%.3f callsPerSec=%.1f "
                 "maxCallMs=%.3f lastSample=%d inkFlushed=%d\n",
                 calls, loopElapsedSec, callsPerSec, maxMs, lastSample, inkFlushed);

    // 佐证本测试确实达到了远高于 test_continuous_input_regression.cpp（因逐次
    // CountInk 被天然限速）的调用频率，仅供人工/评审对照，不作为通过条件的一部分
    // （该数字受宿主 CPU 影响不稳定，见 docs/plans/P7-2.md §4.2）。
    std::fprintf(stderr,
                 "[test_flush_throttle_readback] (informational) achieved call rate should be "
                 "significantly higher than the CountInk-throttled baseline test\n");

    // 1) 单次调用耗时不出现阻塞级尖峰：沿用既有阈值口径。
    CHECK(maxMs < kMaxCallMs, "no single readback call exceeds threshold under hammering");

    // 2) 无长期孔洞：孔洞判据与 test_continuous_input_regression.cpp 同一口径
    //    （<15% 非 ASan / <50% ASan）。
    CHECK(inkFlushed > 0, "flushed canvas has ink");
    const int missing = inkFlushed - lastSample;
    CHECK(missing < inkFlushed * kMissingPercent / 100,
          "last sample before flush converges to complete (missing<threshold)");

    dgcDestroy(ctx);

    if (failures == 0) {
        std::fprintf(stderr, "[test_flush_throttle_readback] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
