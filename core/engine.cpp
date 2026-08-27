#include "core/engine.h"

#include <chrono>
#include <utility>

#include "core/interfaces/i_paint_kernel.h"
#include "core/interfaces/i_render_backend.h"
#include "core/stroke_predictor.h"

namespace {

// 外部入队边界上限：pending_input_ 超过该值即返回 false（调用方可重试/丢弃），
// 避免生产者快于消费者时无界膨胀。
constexpr std::size_t kMaxPendingInput = 1024;

}  // namespace

Engine::Engine(IPaintKernel* kernel, IRenderBackend* backend)
    : kernel_(kernel), backend_(backend) {}

Engine::~Engine() {
    stop();
}

bool Engine::running() const noexcept {
    return running_.load(std::memory_order_acquire);
}

void Engine::start() {
    // 幂等：已启动则直接返回。
    if (running_.load(std::memory_order_acquire)) {
        return;
    }
    stop_.store(false, std::memory_order_release);
    // 默认笔刷同步建于调用线程（start() 返回前即存在），消除「C API 设色早于
    // brushLoop 异步创建笔刷」的竞态（风险 R3）：brushLoop 不再自行 createBrush，
    // 复用 default_brush_。
    default_brush_ = kernel_->createBrush(BrushParams{});
    input_thread_  = std::thread(&Engine::inputLoop, this);
    brush_thread_  = std::thread(&Engine::brushLoop, this);
    render_thread_ = std::thread(&Engine::renderLoop, this);
    running_.store(true, std::memory_order_release);
}

void Engine::stop() {
    // 幂等：置停标志 → 唤醒输入线程 → 依次 join 三线程。消费循环每轮先判 stop_，
    // SPSC 消费侧均为「yield + stop_ 检查」的有界等待，保证 stop() 不卡死。
    stop_.store(true, std::memory_order_release);
    input_cv_.notify_all();
    if (input_thread_.joinable())  input_thread_.join();
    if (brush_thread_.joinable())  brush_thread_.join();
    if (render_thread_.joinable()) render_thread_.join();
    running_.store(false, std::memory_order_release);
}

void Engine::setPredictor(std::unique_ptr<StrokeModeler> predictor) {
    predictor_ = std::move(predictor);
}

bool Engine::submitInput(const StrokeEvent& ev) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (pending_input_.size() >= kMaxPendingInput) {
        return false;
    }
    pending_input_.push_back(ev);
    // B5-2：仅 StrokePoint 事件同步递增 submitted_（与渲染线程 composite 轮数一一对应；
    // Begin/End 不产 stamp 批，不计入，否则 flush 屏障永远追不平）。
    if (ev.type == StrokeEventType::StrokePoint) {
        submitted_.fetch_add(1, std::memory_order_relaxed);
    }
    input_cv_.notify_one();
    return true;
}

void Engine::flush() {
    // drain 屏障（B5-2）：把三线程异步时序压成确定性 drain。仅在引擎三线程之外调用。
    //
    // 批量 composite：先置位 flush_requested_，渲染线程据此把已攒批尽早合入一次提交，
    // 保证「生产者持续投递、队列不空」时屏障仍能等来 composite（避免饿死）。
    flush_requested_.store(true, std::memory_order_release);
    //
    // 1) 等 pending_input_ 排空：短锁检查 + 解锁 + yield 轮询。绝不持 input_mutex_ 自旋，
    //    否则与 inputLoop 抢锁可能死锁（review 反馈 1）。
    while (true) {
        {
            std::lock_guard<std::mutex> lock(input_mutex_);
            if (pending_input_.empty()) {
                break;
            }
        }
        std::this_thread::yield();
    }
    // 2) 等两段 SPSC 排空（事件从 pending_input_ → input_to_brush_ → brush_to_render_ 走完）。
    while (!input_to_brush_.empty() || !brush_to_render_.empty()) {
        std::this_thread::yield();
    }
    // 3) 等渲染线程把每个已提交 StrokePoint 的 stamp 批都 composite 完（epoch 屏障）。
    while (composited_.load(std::memory_order_acquire) !=
           submitted_.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
}

void Engine::inputLoop() {
    // 输入线程：从线程安全的 pending_input_（mutex + condvar）取事件，
    // 搬进无锁 SPSC 段 input_to_brush_，保证其仍是单生产者 × 单消费者。
    // B1-5 设计预留：predictor_ 非空时，对真实 StrokePoint 走 Update/Predict
    // 产出平滑 + 预测点；Begin/End 触发 Reset。默认 nullptr 保持原样透传。
    auto pushEvent = [this](const StrokeEvent& ev) {
        while (!input_to_brush_.try_push(ev)) {
            if (stop_.load(std::memory_order_acquire)) {
                return false;
            }
            std::this_thread::yield();
        }
        return true;
    };
    while (true) {
        StrokeEvent ev{};
        {
            std::unique_lock<std::mutex> lock(input_mutex_);
            input_cv_.wait(lock, [this] {
                return stop_.load(std::memory_order_acquire) || !pending_input_.empty();
            });
            if (stop_.load(std::memory_order_acquire) && pending_input_.empty()) {
                return;
            }
            ev = pending_input_.front();
            pending_input_.pop_front();
        }

        if (predictor_) {
            if (ev.type == StrokeEventType::BeginStroke ||
                ev.type == StrokeEventType::EndStroke) {
                predictor_->Reset();
            } else if (ev.type == StrokeEventType::StrokePoint) {
                std::vector<StrokePoint> out;
                predictor_->Update(ev.point, &out);
                predictor_->Predict(&out);
                for (const StrokePoint& p : out) {
                    if (!pushEvent(StrokeEvent{StrokeEventType::StrokePoint, p})) {
                        return;
                    }
                }
                continue;  // 原始真实点已含于 out（平滑点），不再重复透传。
            }
        }

        if (!pushEvent(ev)) {
            return;
        }
    }
}

void Engine::brushLoop() {
    // 内核线程：beginStroke/endStroke 有状态，必须在此线程执行。
    // 默认笔刷已在 start() 同步创建（default_brush_），此处直接复用，不再重复 createBrush。
    const BrushHandle brush = default_brush_;
    while (true) {
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
        StrokeEvent ev{};
        if (!input_to_brush_.try_pop(ev)) {
            std::this_thread::yield();
            continue;
        }
        switch (ev.type) {
            case StrokeEventType::BeginStroke:
                kernel_->beginStroke(brush, ev.point);
                break;
            case StrokeEventType::StrokePoint: {
                std::vector<StampData> stamps = kernel_->strokeTo(brush, ev.point);
                // 有界等待：满则 yield 重试，期间响应 stop_。
                while (!brush_to_render_.try_push(std::move(stamps))) {
                    if (stop_.load(std::memory_order_acquire)) {
                        return;
                    }
                    std::this_thread::yield();
                }
                break;
            }
            case StrokeEventType::EndStroke:
                kernel_->endStroke(brush);
                break;
        }
    }
}

void Engine::renderLoop() {
    // 渲染线程：取 stamp 批 → 合批 → composite → present。
    //
    // 批量 composite（性能根因一：每输入点一次 SubmitAndWait 的 ~0.73ms/批同步开销）：
    // 把多个输入点的 stamp 批攒进 batch，满足任一条件才合批提交一次：
    //   - flush() 已请求（drain 屏障在等合成完成，强制尽快提交）；
    //   - brush_to_render_ 已空（当前输入暂告一段落，避免无限攒批 / 延迟上屏）。
    // composited_ 按批内 stamp 批数（= 已提交 StrokePoint 数，含空批）递增，屏障追平语义
    // 不变；空批（未移动的点）也计入，与 submitted_ 一一对应。
    std::vector<std::vector<StampData>> batch;
    auto flushBatch = [&]() {
        if (batch.empty()) {
            return;
        }
        std::vector<StampData> flat;
        {
            std::size_t total = 0;
            for (const auto& b : batch) {
                total += b.size();
            }
            flat.reserve(total);
        }
        for (const auto& b : batch) {
            flat.insert(flat.end(), b.begin(), b.end());
        }
#ifdef DGCPAIN_PERF
        auto r0 = std::chrono::steady_clock::now();
#endif
        backend_->composite(flat);
        backend_->present();
#ifdef DGCPAIN_PERF
        auto r1 = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[PERF] engine::renderLoop composite=%.3f ms stamps=%zu batches=%zu\n",
                     std::chrono::duration<double, std::milli>(r1 - r0).count(), flat.size(),
                     batch.size());
#endif
        // B5-2：每合批提交一轮后递增 composited_（release，供 flush 屏障 acquire 追平）。
        composited_.fetch_add(batch.size(), std::memory_order_release);
        batch.clear();
    };

    while (true) {
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
        std::vector<StampData> stamps;
        if (brush_to_render_.try_pop(stamps)) {
            batch.push_back(std::move(stamps));
            // flush() 在等合成完成：把已攒批尽早合入一次提交（原子交换顺便清标志，幂等）。
            if (flush_requested_.exchange(false, std::memory_order_acq_rel)) {
                flushBatch();
            }
            continue;
        }
        if (!batch.empty()) {
            flushBatch();  // 队列已空：当前输入暂告一段落，合批提交。
            continue;
        }
        std::this_thread::yield();
    }
}
