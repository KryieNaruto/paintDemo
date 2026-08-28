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
    // D6-1（drain 屏障扩容，见 submitted_/composited_ 注释）：predictor 激活后一次
    // 外部 submitInput 的 StrokePoint 会被 Update/Predict 展开成 0..N 个内部事件
    // （resampler 上采样 + 预测点），破坏了「1 次外部提交 == 1 次渲染合批」的
    // 1:1 关系。count_submission 标记「本事件是否代表一次外部提交在 composited_
    // 侧的完成」：passthrough（无 predictor）路径下恒为 true（默认值，保持
    // 1:1）；predictor 展开路径下仅展开出的**最后一个**事件为 true，其余为
    // false——SPSC 单消费者严格 FIFO，故该事件被合成时，同一原始提交展开出的
    // 全部前序事件必已合成，语义仍是「一次提交完成」。
    bool count_submission = true;
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
    // B1-5 交付保持默认 nullptr；D6-1 起由 C API 的 dgcSetBrushSetting(modeler 项) 惰性注入
    // （首次设置才创建并 setPredictor，之后仅 Configure，不再替换/销毁，见 predictor_mutex_ 注释）。
    void setPredictor(std::unique_ptr<StrokeModeler> predictor);

    // drain 屏障（B5-2）：阻塞至「所有已提交输入已合成完毕」。仅在引擎三线程之外
    // （C API 调用线程）调用；纯等待 + 原子读，无新堆所有权。
    void flush();

    // 非阻塞 catch-up（P7-1）：仅对 flush_requested_ 做一次 release store，立即返回，
    // 不等待任何队列/composited_ 追平。用于 GUI/调用线程「顺路」催促渲染线程尽快
    // 找机会合批，不像 flush() 那样阻塞到 drain 完成。可在引擎运行期间任意线程调用，
    // 无堆分配、无锁、无跨线程等待。
    void requestFlush() noexcept;

    // 默认笔刷句柄（D6-3）：start() 内同步创建，返回前即有效，供 C API
    // dgcSetBrushColor(DGC_DEFAULT_BRUSH, ...) 等无需等待 brushLoop 异步创建
    // （消除设色早于笔刷建成的竞态，风险 R3）。start() 前调用返回值未定义。
    BrushHandle defaultBrush() const noexcept { return default_brush_; }

private:
    void inputLoop();             // 输入线程：pending_input_ → input_to_brush_
    void brushLoop();             // 内核线程：strokeTo → stamp 批 → brush_to_render_
    void renderLoop();            // 渲染线程：composite + present

    IPaintKernel*  kernel_;
    IRenderBackend* backend_;

    // 预测器槽（B1-5 设计预留）：RAII 持有，默认 nullptr = passthrough。
    // D6-1：predictor_ 的写入方（setPredictor，C API 调用线程）与读取方
    // （inputLoop，输入线程）并发，predictor_mutex_ 守卫该 unique_ptr 本体
    // （只保护指针的写/读，不保护 StrokeModeler 内部状态——那是
    //  StrokeModeler::Impl 自带的 mutex 的职责，见 stroke_predictor.cpp）。
    // inputLoop 每次取一次裸指针快照（加锁拷贝后立即解锁）；predictor 一旦
    // 注入后不再被替换/销毁，故该裸指针在引擎生命周期内始终有效，可在锁外调用。
    std::mutex predictor_mutex_;
    std::unique_ptr<StrokeModeler> predictor_;

    // brush_to_render_ 队列元素：stamp 批 + count_submission 透传（D6-1，见
    // StrokeEvent::count_submission 注释）——kernel_->strokeTo 的输出批数量与
    // input_to_brush_ 的 StrokePoint 事件数一一对应，count_submission 需从
    // 触发它的 StrokeEvent 原样带到 renderLoop 供 composited_ 计数。
    struct RenderBatch {
        std::vector<StampData> stamps;
        bool count_submission = true;
    };

    // 两段无锁 SPSC 队列
    RingBuffer<StrokeEvent, 1024>  input_to_brush_;
    RingBuffer<RenderBatch, 256>   brush_to_render_;

    // 外部入队边界（线程安全，来自 UI/C API 线程）
    std::deque<StrokeEvent> pending_input_;
    std::mutex             input_mutex_;
    std::condition_variable input_cv_;

    // drain 屏障计数（B5-2；D6-1 扩展支持 predictor 展开）：submitted_ 在
    // submitInput 对外部 StrokePoint 调用同步递增（不受内部展开影响，语义仍是
    // 「外部提交了多少次」）；composited_ 由渲染线程每合批提交后按批内
    // RenderBatch::count_submission == true 的条目数 fetch_add（而非批大小本身），
    // 与展开无关地保持「一次外部提交 == 一次 composited_ 计数」；flush 等二者追平。
    std::atomic<std::size_t> submitted_{0};
    std::atomic<std::size_t> composited_{0};

    // 批量 composite（性能根因一）：renderLoop() 据此把已攒批尽早合入一次提交，避免
    // 「生产者持续投递、队列不空」时屏障等不到 composite 而饿死。三条触发条件（P7-1）：
    //   1) flush_requested_ 被置位——由 flush()（drain 屏障）或 requestFlush()（非阻塞
    //      catch-up，见 dgcReadbackPixels/dgcExportPNG）任一途径 store(true)；
    //   2) 攒批已超上限（stamp 数或时长阈值，见 engine.cpp 匿名命名空间常量）——
    //      不依赖 1)，兜底"队列理论上永不空、无人置位 flush_requested_"的连续输入场景；
    //   3) brush_to_render_ 已空（当前输入暂告一段落）。
    std::atomic<bool> flush_requested_{false};

    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::thread input_thread_;
    std::thread brush_thread_;
    std::thread render_thread_;

    // 默认笔刷句柄（D6-3）：start() 内同步创建（早于三线程启动/join 前对外可见），
    // 值语义，无堆所有权；brushLoop 复用同一句柄，不再自行 createBrush。
    BrushHandle default_brush_ = 0;
};
