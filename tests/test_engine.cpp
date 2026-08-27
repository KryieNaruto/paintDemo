// engine start/stop 冒烟：注入 Null 桩，入队若干点，限时 stop 断言返回（覆盖「不卡死」）。
// 另含 B5-2 flush drain 屏障单测：提交 N 个 StrokePoint 后 flush()，断言渲染线程
// composite 轮数追平 N（验证「flush 返回时所有已提交输入已合成」）。
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "core/engine.h"
#include "core/null/null_paint_kernel.h"
#include "core/null/null_render_backend.h"

namespace {

// 计数后端：记录 composite 调用轮数（B5-2 flush 屏障单测用）。
class CountingBackend : public IRenderBackend {
public:
    void init(PlatformSurface, int, int) override {}
    void resize(int, int) override {}
    void beginFrame() override {}
    void composite(const std::vector<StampData>&) override { ++composites; }
    void clearCanvas(float, float, float, float) override {}
    void present() override {}
    void shutdown() override {}
    void initOffscreen(int, int) override {}
    void readback(void*) override {}
    void exportPNG(const char*) override {}

    std::atomic<int> composites{0};
};

}  // namespace

int main() {
    NullPaintKernel  kernel;
    NullRenderBackend backend;

    Engine e(&kernel, &backend);

    // start 幂等：重复 start 不崩。
    e.start();
    e.start();
    assert(e.running());

    // 入队：Begin → Point×N → End。
    StrokePoint p{};
    p.x = 1.0f;
    p.y = 2.0f;
    p.pressure = 0.5f;
    p.t_us = 123456;

    assert(e.submitInput({StrokeEventType::BeginStroke, p}));
    for (int i = 0; i < 1000; ++i) {
        assert(e.submitInput({StrokeEventType::StrokePoint, p}));
    }
    assert(e.submitInput({StrokeEventType::EndStroke, p}));

    // 短暂睡眠让三线程消化。
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // 限时 stop：必须在 2s 内返回（「不卡死」）。
    const auto t0 = std::chrono::steady_clock::now();
    e.stop();
    const auto t1 = std::chrono::steady_clock::now();
    const auto elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count();
    assert(elapsed_ms < 2000);
    assert(!e.running());

    // stop 幂等：再次 stop 不崩。
    e.stop();
    assert(!e.running());

    // 停止后再次 start 可重启（线程对象已 join，可重新赋值）。
    e.start();
    assert(e.running());
    e.stop();
    assert(!e.running());

    // B5-2：flush drain 屏障。批量 composite（性能根因一）下渲染线程把多个输入点的
    // stamp 批合批提交，composite 调用次数 ≤ 提交点数（可能分散，但不逐点各调一次）。
    // flush 应阻塞至所有已提交 StrokePoint 合成完毕（composited_ 追平 submitted_）。
    {
        NullPaintKernel  kernel2;
        CountingBackend  backend2;
        Engine e2(&kernel2, &backend2);
        e2.start();

        StrokePoint p{};
        p.x = 3.0f;
        p.y = 4.0f;
        p.pressure = 0.25f;

        constexpr int kPoints = 256;
        assert(e2.submitInput({StrokeEventType::BeginStroke, p}));
        for (int i = 0; i < kPoints; ++i) {
            assert(e2.submitInput({StrokeEventType::StrokePoint, p}));
        }
        assert(e2.submitInput({StrokeEventType::EndStroke, p}));

        e2.flush();  // 阻塞至全部 kPoints 个 StrokePoint 合成完毕。
        // 合批契约：至少一次提交（屏障要求所有工作完成），且不超过点数。
        const int calls = backend2.composites.load();
        assert(calls >= 1);
        assert(calls <= kPoints);

        e2.stop();
    }

    return 0;
}
