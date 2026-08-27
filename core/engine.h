#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "core/ring_buffer.h"
#include "core/types.h"

class IPaintKernel;
class IRenderBackend;
class StrokeModeler;

// 输入事件：内核线程的 beginStroke/endStroke 有状态，必须在内核线程执行，
// 故输入队列元素是完整事件（含 Begin/Point/End）而非裸 StrokePoint。
enum class StrokeEventType { BeginStroke, StrokePoint, EndStroke };

struct StrokeEvent {
    StrokeEventType type;
    StrokePoint point;
};

// Engine：Input → Brush → Render 三线程编排 + 两段无锁 SPSC 队列。
//
//   外部(C API/平台线程) --submitInput--> pending_input_(mutex+condvar)
//        --输入线程--> input_to_brush_(SPSC) --内核线程--> brush_to_render_(SPSC)
//        --渲染线程--> backend_->composite/present
//
// 外部入队先落线程安全的 pending_input_，再由输入线程搬进 SPSC 段，保证
// input_to_brush_ 仍是「单生产者 × 单消费者」，SPSC 约束成立。
class Engine {
public:
    Engine(IPaintKernel* kernel, IRenderBackend* backend);  // 注入，不拥有
    ~Engine();                                             // 保证 stop()
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    void start();                 // 幂等：已启动则直接返回
    void stop();                  // 幂等：置停标志 → 唤醒 → join 三线程，不卡死
    bool running() const noexcept;

    // 外部入队口（B1-4 从这里接 C API）。满则返回 false（调用方可重试/丢弃）。
    bool submitInput(const StrokeEvent& ev);

    // 可注入预测器槽（B1-5 设计预留）：默认 nullptr = passthrough（inputLoop 原样透传）。
    // 非空时输入线程对真实 StrokePoint 走 Update/Predict 产出平滑 + 预测点；Begin/End 触发 Reset。
    // B1-5 交付保持默认 nullptr，真实接线归 B2/B3。
    void setPredictor(std::unique_ptr<StrokeModeler> predictor);

    // drain 屏障（B5-2）：阻塞至「所有已提交输入已合成完毕」。仅在引擎三线程之外
    // （C API 调用线程）调用；纯等待 + 原子读，无新堆所有权。
    void flush();

private:
    void inputLoop();             // 输入线程：pending_input_ → input_to_brush_
    void brushLoop();             // 内核线程：strokeTo → stamp 批 → brush_to_render_
    void renderLoop();            // 渲染线程：composite + present

    IPaintKernel*  kernel_;
    IRenderBackend* backend_;

    // 预测器槽（B1-5 设计预留）：RAII 持有，默认 nullptr = passthrough。
    std::unique_ptr<StrokeModeler> predictor_;

    // 两段无锁 SPSC 队列
    RingBuffer<StrokeEvent, 1024>            input_to_brush_;
    RingBuffer<std::vector<StampData>, 256>  brush_to_render_;

    // 外部入队边界（线程安全，来自 UI/C API 线程）
    std::deque<StrokeEvent> pending_input_;
    std::mutex             input_mutex_;
    std::condition_variable input_cv_;

    // drain 屏障计数（B5-2）：submitted_ 在 submitInput 对 StrokePoint 事件同步递增；
    // composited_ 由渲染线程每合批提交后 fetch_add(批内点数)；flush 等二者追平。
    std::atomic<std::size_t> submitted_{0};
    std::atomic<std::size_t> composited_{0};

    // 批量 composite（性能根因一）：flush() 置位，渲染线程据此把已攒批尽早合入一次提交，
    // 避免「生产者持续投递、队列不空」时屏障等不到 composite 而饿死。
    std::atomic<bool> flush_requested_{false};

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread input_thread_;
    std::thread brush_thread_;
    std::thread render_thread_;
};
