// P7-1 回归测试：真正连续不间断输入下的孔洞修复 + GUI 侧读回无阻塞尖峰。
//
// 背景（详见 docs/tasks/detail/帧率与读回孔洞修复.md）：`d64c2fa` 用阻塞 engine->flush()
// 修孔洞 → PC 20fps 回退；`f96456e` 移除阻塞 flush 引入快照缓存 → PC fps 恢复但连续
// 输入下队列理论上永不空、renderLoop 的两条原有触发条件（flush_requested_ 置位 /
// 队列已空）都等不到，无界攒批 → 缓存长期滞后、孔洞复现。P7-1 让 dgcReadbackPixels/
// dgcExportPNG 对 flush_requested_ 做非阻塞 catch-up，并给 renderLoop 补攒批上限兜底
// （stamp 数 + 时长双阈值），两者共同保证连续输入下缓存滞后有界。
//
// 本用例是仓库首次让"提交线程"与"读回线程"真并发运行：后台线程 producer 紧凑循环
// 提交（不留空闲间隙，每点间 sleep 200~500us，SPSC 队列理论上不应观测为空），前台
// （main）线程持续 dgcReadbackPixels 采样并记时。
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "dgc_paint_c_api.h"

// ASan/LSan（DGCPAIN_SANITIZE=ON，见 CMakeLists.txt:33-41）对每次内存访问插桩，实测
// 引擎三线程整体吞吐可下降一个数量级以上，而 producer 线程的 sleep_for 睡眠时长是
// 真实墙钟时间、不受插桩影响——两者不对称会使消费者相对生产者更滞后。这里放宽的是
// 由插桩开销主导的时间类/滞后占比阈值，不改变断言验证的架构性质（"非阻塞 + 有界
// 滞后"），符合计划 §6 风险 3 的预案（沿用既有 test_perf_regression.cpp 的宽松阈值
// 风格，按环境放宽数倍安全边际）。
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
constexpr double kMaxCallMs = 150.0;      // 非 ASan 下 20ms；插桩下放宽约 7.5 倍。
constexpr int kMissingPercent = 50;       // 非 ASan 下 15%；插桩下放宽约 3.3 倍。
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
            // 白底 (255,255,255)：非白 = 墨迹。
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
    constexpr int kPoints = 700;  // 覆盖数百次点，贯穿画布的螺旋路径。

    DgcContext* ctx = dgcCreate();
    CHECK(ctx != nullptr, "dgcCreate non-null");
    if (ctx == nullptr) return 1;

    CHECK(dgcSetOffscreenSurface(ctx, kW, kH) == DGC_OK, "offscreen init OK");
    CHECK(dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f) == DGC_OK, "clear white");

    std::atomic<bool> producerDone{false};

    // 后台线程：真正连续不间断提交——每点间 sleep 200~500us（≤1ms 节奏），全程
    // 不调用 dgcFlush，覆盖一条贯穿画布的螺旋路径，确保各处都有墨迹可验证。
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

    // 前台线程（模拟 GUI 主循环）：在 producer 收尾前持续 dgcReadbackPixels 采样，
    // 记每次调用耗时，断言不出现阻塞级尖峰。
    std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 0);
    std::vector<double> callMs;
    std::vector<int> inkSamples;
    callMs.reserve(2000);
    inkSamples.reserve(2000);
    int lastSample = 0;
    while (!producerDone.load(std::memory_order_acquire)) {
        auto t0 = std::chrono::steady_clock::now();
        CHECK(dgcReadbackPixels(ctx, buf.data()) == DGC_OK, "readback during continuous input OK");
        auto t1 = std::chrono::steady_clock::now();
        callMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        lastSample = CountInk(buf, kW, kH);
        inkSamples.push_back(lastSample);
    }
    producer.join();  // RAII：显式 join，不 detach。

    // 收尾后再采一次样，覆盖 producer 刚结束、消费者还未来得及追上的窗口。
    {
        auto t0 = std::chrono::steady_clock::now();
        CHECK(dgcReadbackPixels(ctx, buf.data()) == DGC_OK, "readback right after producer join OK");
        auto t1 = std::chrono::steady_clock::now();
        callMs.push_back(std::chrono::duration<double, std::milli>(t1 - t0).count());
        lastSample = CountInk(buf, kW, kH);
        inkSamples.push_back(lastSample);
    }

    // 权威对照：dgcFlush + 读回 = 完整画布。
    CHECK(dgcFlush(ctx) == DGC_OK, "flush OK");
    std::vector<std::uint8_t> bufFlushed((size_t)kW * kH * 4, 0);
    CHECK(dgcReadbackPixels(ctx, bufFlushed.data()) == DGC_OK, "readback after flush OK");
    const int inkFlushed = CountInk(bufFlushed, kW, kH);

    // 1) GUI 侧单次调用耗时不出现阻塞级尖峰：沿用 test_perf_regression.cpp 的
    //    50fps 安全阈值口径（<20ms），远低于历史阻塞病理（d64c2fa 前 max 41~104ms）。
    double maxMs = 0;
    for (double v : callMs) {
        if (v > maxMs) maxMs = v;
    }
    std::fprintf(stderr,
                 "[test_continuous_input_regression] samples=%zu maxCallMs=%.3f "
                 "lastSample=%d inkFlushed=%d\n",
                 inkSamples.size(), maxMs, lastSample, inkFlushed);
    CHECK(maxMs < kMaxCallMs, "no single readback call exceeds threshold under continuous input");

    // 2) 无长期孔洞：最后一次（producer join 后）采样值与权威完整画布差值占比低于阈值
    //    （非 ASan 下 <15%，比 test_readback_drain 的 <5% 略宽松，因为这是"生产者刚
    //    结束、消费者还未来得及追上"的更严苛并发场景；ASan 插桩下进一步放宽，见上）。
    CHECK(inkFlushed > 0, "flushed canvas has ink");
    const int missing = inkFlushed - lastSample;
    CHECK(missing < inkFlushed * kMissingPercent / 100,
          "last sample before flush converges to complete (missing<threshold)");

    // 3) 不长期卡在低墨迹量不动：后半程采样均值应显著高于前半程（捕捉"长期滞后"
    //    的孔洞病理，而非仅看最终值）。
    CHECK(inkSamples.size() >= 4, "enough samples collected during continuous input");
    if (inkSamples.size() >= 4) {
        const size_t half = inkSamples.size() / 2;
        double firstHalfAvg = 0, secondHalfAvg = 0;
        for (size_t i = 0; i < half; ++i) firstHalfAvg += inkSamples[i];
        for (size_t i = half; i < inkSamples.size(); ++i) secondHalfAvg += inkSamples[i];
        firstHalfAvg /= (double)half;
        secondHalfAvg /= (double)(inkSamples.size() - half);
        std::fprintf(stderr,
                     "[test_continuous_input_regression] firstHalfAvg=%.1f secondHalfAvg=%.1f\n",
                     firstHalfAvg, secondHalfAvg);
        CHECK(secondHalfAvg >= firstHalfAvg,
              "ink samples trend upward over continuous input (no long-term stall)");
    }

    dgcDestroy(ctx);

    if (failures == 0) {
        std::fprintf(stderr, "[test_continuous_input_regression] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
