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
    // D6-1：与 inputLoop 的读并发，predictor_mutex_ 守卫指针写入（§4.3 点 1）。
    std::lock_guard<std::mutex> lock(predictor_mutex_);
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

        // D6-1：每事件加锁取一次裸指针快照后立即解锁（predictor 注入后不再替换/
        // 销毁，故裸指针在引擎生命周期内有效，可在锁外安全调用；StrokeModeler
        // 自身的线程安全由其内部 mutex 负责，见 stroke_predictor.cpp）。
        StrokeModeler* pred = nullptr;
        {
            std::lock_guard<std::mutex> lock(predictor_mutex_);
            pred = predictor_.get();
        }
        if (pred != nullptr) {
            if (ev.type == StrokeEventType::BeginStroke ||
                ev.type == StrokeEventType::EndStroke) {
                pred->Reset();
            } else if (ev.type == StrokeEventType::StrokePoint) {
                std::vector<StrokePoint> out;
                pred->Update(ev.point, &out);
                pred->Predict(&out);
                // D6-1：一次外部 StrokePoint 提交展开成 out.size() 个内部事件，
                // 破坏了「1 提交 = 1 composited_ 计数」的 1:1 关系（见
                // StrokeEvent::count_submission 注释）。只把展开出的最后一个
                // 事件标记为 count_submission=true，其余全为 false：SPSC 单消费者
                // 严格 FIFO，故它被合成时，本次提交展开出的全部前序事件必已合成。
                // Update()/Predict() 的管线设计下 out 恒非空（resampler 首点必发、
                // 稀疏/密集分支均至少落 1 点），故无需处理 out.empty() 的兜底路径；
                // 若未来管线改动导致可能为空，需在此补发一个 count-only 事件，
                // 否则 submitted_/composited_ 将永久失配、flush() 卡死。
                for (std::size_t i = 0; i < out.size(); ++i) {
                    StrokeEvent oev{StrokeEventType::StrokePoint, out[i],
                                    /*count_submission=*/(i + 1 == out.size())};
                    if (!pushEvent(oev)) {
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
                // count_submission 原样透传（D6-1）：从触发本批的 StrokeEvent 带到
                // renderLoop，供 composited_ 按「外部提交数」而非「批数」计数。
                RenderBatch batch_item{std::move(stamps), ev.count_submission};
                // 有界等待：满则 yield 重试，期间响应 stop_。
                while (!brush_to_render_.try_push(std::move(batch_item))) {
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
    // composited_ 按批内 count_submission==true 的条目数（D6-1：= 外部 StrokePoint
    // 提交次数，而非批数本身，见 StrokeEvent::count_submission / RenderBatch 注释）
    // 递增，屏障追平语义不变；predictor 关闭（passthrough）时 count_submission
    // 恒为 true，批数即提交数，与改造前行为完全一致（零回归）。
    std::vector<RenderBatch> batch;
    auto flushBatch = [&]() {
        if (batch.empty()) {
            return;
        }
        std::vector<StampData> flat;
        std::size_t counted = 0;
        {
            std::size_t total = 0;
            for (const auto& b : batch) {
                total += b.stamps.size();
                if (b.count_submission) {
                    ++counted;
                }
            }
            flat.reserve(total);
        }
        for (const auto& b : batch) {
            flat.insert(flat.end(), b.stamps.begin(), b.stamps.end());
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
        // B5-2/D6-1：每合批提交一轮后按「外部提交计数」递增 composited_
        // （release，供 flush 屏障 acquire 追平），而非批内条目总数。
        composited_.fetch_add(counted, std::memory_order_release);
        batch.clear();
    };

    while (true) {
        if (stop_.load(std::memory_order_acquire)) {
            return;
        }
        RenderBatch stamps;
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
