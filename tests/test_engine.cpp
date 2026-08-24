// engine start/stop 冒烟：注入 Null 桩，入队若干点，限时 stop 断言返回（覆盖「不卡死」）。
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>

#include "core/engine.h"
#include "core/null/null_paint_kernel.h"
#include "core/null/null_render_backend.h"

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

    return 0;
}
