// P7-2 白盒回归测试（Engine 级）：高频 requestFlush() 不应把批量 composite 打回
// 接近逐条处理的水平。
//
// 背景（详见 docs/plans/P7-2.md §0.3/§4.1）：dgcReadbackPixels/dgcExportPNG 对渲染
// 线程的全部影响等价于一次 Engine::requestFlush()（C API 层不额外触碰
// flush_requested_ 之外的任何渲染线程状态）。故本用例直接白盒复用 core/engine.h，
// 用一个"不加任何 sleep、紧凑循环调用 requestFlush()"的 hammer 线程模拟"忙循环调用
// dgcReadbackPixels 所能达到的最高频率"这一理论上限场景，验证 P7-2 新增的
// kMinFlushIntervalMs 节流确实生效：平均批大小不退化到个位数低端（P7-1 回归时的
// 病理是平均批大小趋近 1），且节流后的 flush 响应频率有可量化的上界。
//
// producer 节奏说明（执行阶段实测修正，见 docs/plans/P7-2.md §4.1 与 §8 风险 4）：
// renderLoop() 既有触发条件 3)（brush_to_render_ 已空即无条件立即 flush，P7-1 起即
// 存在、本任务未改动）在"生产者按固定 sleep 逐点提交、渲染循环单次迭代开销远小于
// 该 sleep 间隔"的模式下会在几乎每次成功 try_pop 后的下一次 try_pop 就观测到队列
// 已空，从而在"批大小"层面独立于 flush_requested_ 是否被节流都把批量打回到个位数
// ——这是渲染循环轮询本身固有的时序特性，与 kMinFlushIntervalMs 节流是否生效正交，
// 与"引擎在连续繁忙输入下 brush_to_render_ 理论上永不为空"（core/engine.cpp 注释）
// 描述的场景不符。故本用例按 engine.cpp 该注释字面场景构造 producer：不使用固定
// sleep 节奏，改为"重试直到 submitInput 成功"的背压驱动持续提交（无 CHECK 断言
// 单次提交是否成功——队列满时重试属正常背压语义，见 Engine::submitInput 文档），
// 使 brush_to_render_ 在整个测试窗口内保持有积压，让 kMaxBatchDurationMs/
// kMaxBatchStamps（既有攒批上限）与本任务新增的 kMinFlushIntervalMs 节流真正成为
// 决定批大小的主导因素，而不是被条件 3) 抢跑。
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#include "core/engine.h"
#include "core/interfaces/i_paint_kernel.h"
#include "core/interfaces/i_render_backend.h"

namespace {

// OneStampKernel：每次 strokeTo 恰好产出 1 个 StampData（区别于默认返回空 vector 的
// NullPaintKernel），使"批内 stamp 总数"与"批内 StrokePoint 提交次数"严格一致，
// 让 §4.1 依据的"点数/秒 × 节流窗口 = 期望批大小"数值推导可以直接对照实测结果。
class OneStampKernel : public IPaintKernel {
public:
    BrushHandle createBrush(const BrushParams&) override { return 0; }
    void beginStroke(BrushHandle, const StrokePoint&) override {}
    std::vector<StampData> strokeTo(BrushHandle, const StrokePoint& p) override {
        StampData s{};
        s.x = p.x;
        s.y = p.y;
        s.radius = 1.0f;
        s.hardness = 1.0f;
        s.opacity = 1.0f;
        return {s};
    }
    void endStroke(BrushHandle) override {}
    void setBrushColor(BrushHandle, float, float, float, float) override {}
    void setBrushSetting(BrushHandle, brush::SettingId, float) override {}
};

// BatchCountingBackend：记录 composite() 调用次数与每次调用的 stamp 数总和，
// 用于算平均批大小（avgBatchSize = totalStamps / compositeCalls）。composite()
// 内模拟一次真实后端 GPU SubmitAndWait 的开销（[[sdk-perf-bottlenecks]] 记录的批量
// composite 优化根因数据：单次提交约 0.73ms；docs/plans/P7-2.md §1.3 引用同一数字）
// ——若不模拟该开销，renderLoop() 在零成本 composite() 下每轮循环耗时远低于
// producer 200~500us 的点间隔，会导致每次 try_pop 后紧接着的下一次 try_pop 几乎
// 必然观测为空（生产者尚未来得及提交下一点），触发条件 3)（队列已空，无条件立即
// flush），使平均批大小恒定退化到 1 附近——这是"渲染循环轮询本身的开销"造成的伪
// 现象，与本任务验证的 kMinFlushIntervalMs 节流是否生效无关；加入与真实后端量级
// 一致的模拟开销后，renderLoop 的实际处理节奏才能反映真实 Vulkan 后端下的行为
// （见 test_flush_throttle_readback.cpp 用真实 VkBackend 的黑盒互补验证）。
class BatchCountingBackend : public IRenderBackend {
public:
    void init(PlatformSurface, int, int) override {}
    void resize(int, int) override {}
    void beginFrame() override {}
    void composite(const std::vector<StampData>& stamps) override {
        std::this_thread::sleep_for(std::chrono::microseconds(700));
        ++compositeCalls;
        totalStamps.fetch_add(stamps.size(), std::memory_order_relaxed);
    }
    void clearCanvas(float, float, float, float) override {}
    void present() override {}
    void shutdown() override {}
    void initOffscreen(int, int) override {}
    void readback(void*) override {}
    void exportPNG(const char*) override {}

    std::atomic<std::size_t> compositeCalls{0};
    std::atomic<std::size_t> totalStamps{0};
};

}  // namespace

static int failures = 0;
#define CHECK(cond, name)                            \
    do {                                              \
        if (!(cond)) {                                \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                                \
        }                                              \
    } while (0)

int main() {
    OneStampKernel kernel;
    BatchCountingBackend backend;
    Engine engine(&kernel, &backend);
    engine.start();

    // kPoints 足够大以覆盖多个 kMinFlushIntervalMs(4ms) 窗口的统计样本；配合下方
    // 背压驱动提交（无 sleep，持续重试直到队列腾出空间），brush_to_render_ 预期
    // 在整个窗口内保持有积压（见上方 producer 节奏说明）。
    constexpr int kPoints = 20000;

    std::atomic<bool> hammerDone{false};

    // hammer 线程：从 producer 启动到结束全程，不加任何 sleep，紧凑循环调用
    // requestFlush()，模拟"能达到的最高频率"（真实忙循环 dgcReadbackPixels 的上限
    // 就是 CPU 能跑多快，这里更快、更接近该理论上限）。
    std::thread hammer([&]() {
        while (!hammerDone.load(std::memory_order_acquire)) {
            engine.requestFlush();
        }
    });

    // 背压驱动提交辅助：submitInput 满则返回 false（Engine 既有文档化契约，见
    // core/engine.h「满则调用方可重试/丢弃」），此处选择重试（yield 让出时间片，
    // 不忙自旋抢占渲染/内核线程 CPU），持续提交直到成功——不对单次提交是否一次成功
    // 做断言（队列满属正常背压，不是错误）。
    auto submitRetrying = [&](const StrokeEvent& ev) {
        while (!engine.submitInput(ev)) {
            std::this_thread::yield();
        }
    };

    const auto producerStart = std::chrono::steady_clock::now();
    std::thread producer([&]() {
        StrokePoint p{};
        p.pressure = 0.5f;
        submitRetrying({StrokeEventType::BeginStroke, p});
        for (int i = 0; i < kPoints; ++i) {
            p.x = static_cast<float>(i % 512);
            p.y = static_cast<float>((i * 3) % 512);
            submitRetrying({StrokeEventType::StrokePoint, p});
        }
        submitRetrying({StrokeEventType::EndStroke, p});
    });

    // 在 producer/hammer 均仍在运行期间调用一次 flush()（drain 屏障），断言在有界
    // 时间内返回——证明节流没有引入新的无界等待（P7-2 硬要求）。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    {
        const auto t0 = std::chrono::steady_clock::now();
        engine.flush();
        const auto t1 = std::chrono::steady_clock::now();
        const auto elapsedMs =
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
        std::fprintf(stderr,
                     "[test_flush_throttle_engine] mid-run flush() elapsed=%lldms\n",
                     static_cast<long long>(elapsedMs));
        CHECK(elapsedMs < 2000, "flush() during concurrent hammer returns within bound");
    }

    producer.join();  // RAII：显式 join，不 detach。
    const auto producerEnd = std::chrono::steady_clock::now();
    const double producerDurationMs =
        std::chrono::duration<double, std::milli>(producerEnd - producerStart).count();

    hammerDone.store(true, std::memory_order_release);
    hammer.join();  // RAII：显式 join，不 detach。

    engine.stop();  // 触发最后一批 flushBatch。

    const std::size_t compositeCalls = backend.compositeCalls.load();
    const std::size_t totalStamps = backend.totalStamps.load();
    const double avgBatchSize =
        compositeCalls == 0 ? 0.0 : static_cast<double>(totalStamps) / compositeCalls;

    // kMinFlushIntervalMs/kMaxBatchStamps 与 core/engine.cpp 保持一致，复制而非
    // #include 内部匿名命名空间常量（该常量非公开符号）。
    constexpr double kMinFlushIntervalMsValue = 4.0;
    constexpr double kMaxBatchStampsValue = 512.0;
    constexpr double kMinAvgBatchSize = 4.0;
    // compositeCalls 上界推导（OR 论证，非拍脑袋估算）：renderLoop() 每次 flush 前
    // 必满足 batchStampCount>=kMaxBatchStamps 或 (now-batchStart)>=kMaxBatchDurationMs
    // 或 (haveRequest && intervalOk) 三者之一；后两者都要求"距上次 flush 已过至少
    // kMinFlushIntervalMs（=kMaxBatchDurationMs）"，故可归为同一类「时间驱动」flush，
    // 在 producerDurationMs 内至多发生 producerDurationMs/kMinFlushIntervalMs 次
    // （+1 个边界项）；「stamp 数驱动」flush 每次至少消耗 kMaxBatchStamps 个 stamp，
    // 在 totalStamps 个 stamp 内至多发生 totalStamps/kMaxBatchStamps 次（+1 个边界
    // 项，覆盖不足 512 的收尾批）。两类互斥归因、可直接相加成合法上界；1.5 倍留给
    // 调度抖动/线程唤醒延迟的安全边际。
    const double timeDrivenBound = producerDurationMs / kMinFlushIntervalMsValue + 1.0;
    const double stampDrivenBound =
        static_cast<double>(totalStamps) / kMaxBatchStampsValue + 1.0;
    const double maxCompositeCalls = (timeDrivenBound + stampDrivenBound) * 1.5;

    std::fprintf(stderr,
                 "[test_flush_throttle_engine] producerDurationMs=%.2f compositeCalls=%zu "
                 "totalStamps=%zu avgBatchSize=%.2f maxCompositeCallsBound=%.2f\n",
                 producerDurationMs, compositeCalls, totalStamps, avgBatchSize,
                 maxCompositeCalls);

    CHECK(compositeCalls >= 1, "at least one composite call happened");
    CHECK(avgBatchSize >= kMinAvgBatchSize,
          "average batch size not degraded to near-per-stamp processing under hammering");
    CHECK(static_cast<double>(compositeCalls) <= maxCompositeCalls,
          "composite call frequency bounded by throttled flush_requested_ response rate");

    if (failures == 0) {
        std::fprintf(stderr, "[test_flush_throttle_engine] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
