#pragma once

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>

// 无锁 SPSC 环形队列：单生产者 + 单消费者，满/空/环绕语义由单调计数器推导。
//
// - Capacity 必须是 2 的幂，下标 = 计数 % Capacity。
// - head_ / tail_ 是单调递增计数器（不依赖实际回绕），因此 push/pop 累计超过
//   Capacity 是自然行为（即「环绕」）。
// - empty() == (tail == head)，full() == (tail - head == Capacity)。
// - SPSC 下 try_push / try_pop 无锁、无 CAS：生产者写槽后 release 发布 tail_，
//   消费者 acquire 读 head_ 后读槽，消费完 release 发布 head_。
// - clear() 仅供单线程复位（如 engine stop 后），不承诺并发安全。
template <typename T, std::size_t Capacity>
class RingBuffer {
public:
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be power of two");
    static_assert(Capacity > 0, "Capacity must be positive");

    RingBuffer() = default;
    RingBuffer(const RingBuffer&) = delete;
    RingBuffer& operator=(const RingBuffer&) = delete;

    // 满返回 false，否则写入并返回 true。仅生产者线程可调用。
    bool try_push(const T& v) {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        const std::size_t head = head_.load(std::memory_order_acquire);
        if (tail - head >= Capacity) {
            return false;
        }
        buf_[tail & (Capacity - 1)] = v;
        tail_.store(tail + 1, std::memory_order_release);
        return true;
    }

    // 空返回 false，否则读出并返回 true。仅消费者线程可调用。
    bool try_pop(T& out) {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        if (tail == head) {
            return false;
        }
        out = buf_[head & (Capacity - 1)];
        head_.store(head + 1, std::memory_order_release);
        return true;
    }

    // 以下只读查询仅反映「某次采样」的瞬时状态，供单测/调试使用。
    bool empty() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail == head;
    }

    bool full() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head >= Capacity;
    }

    std::size_t size() const noexcept {
        const std::size_t tail = tail_.load(std::memory_order_acquire);
        const std::size_t head = head_.load(std::memory_order_acquire);
        return tail - head;
    }

    // 仅供单线程复位（并发调用非安全）。
    void clear() noexcept {
        tail_.store(0, std::memory_order_relaxed);
        head_.store(0, std::memory_order_relaxed);
    }

private:
    std::array<T, Capacity> buf_{};
    std::atomic<std::size_t> head_{0};  // 消费者游标（单调递增）
    std::atomic<std::size_t> tail_{0};  // 生产者游标（单调递增）
};
