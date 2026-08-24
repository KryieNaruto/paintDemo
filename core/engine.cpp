#include "core/engine.h"

#include <utility>

#include "core/interfaces/i_paint_kernel.h"
#include "core/interfaces/i_render_backend.h"

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

bool Engine::submitInput(const StrokeEvent& ev) {
    std::lock_guard<std::mutex> lock(input_mutex_);
    if (pending_input_.size() >= kMaxPendingInput) {
        return false;
    }
    pending_input_.push_back(ev);
    input_cv_.notify_one();
    return true;
}

void Engine::inputLoop() {
    // 输入线程：从线程安全的 pending_input_（mutex + condvar）取事件，
    // 搬进无锁 SPSC 段 input_to_brush_，保证其仍是单生产者 × 单消费者。
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
        while (!input_to_brush_.try_push(ev)) {
            if (stop_.load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::yield();
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
    }
}
