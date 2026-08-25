#include "core/engine.h"

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
    const BrushHandle brush = kernel_->createBrush(BrushParams{});
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
    // 渲染线程：取 stamp 批 → composite → present。
    while (true) {
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
        std::vector<StampData> stamps;
        if (!brush_to_render_.try_pop(stamps)) {
            std::this_thread::yield();
            continue;
        }
        backend_->composite(stamps);
        backend_->present();
        // B5-2：每 composite 一轮后递增 composited_（release，供 flush 屏障 acquire 追平）。
        composited_.fetch_add(1, std::memory_order_release);
    }
}
