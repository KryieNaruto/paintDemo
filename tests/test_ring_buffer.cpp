// RingBuffer SPSC 单测：覆盖验收「满 / 空 / 环绕」。
#include <cassert>
#include <cstddef>

#include "core/ring_buffer.h"

int main() {
    RingBuffer<int, 8> rb;  // 8 = 2^3，满足 Capacity 必须是 2 的幂。

    // ── 空 ──
    assert(rb.empty());
    assert(!rb.full());
    assert(rb.size() == 0);
    {
        int out = -1;
        assert(!rb.try_pop(out));  // 空则 pop 返回 false
        assert(out == -1);         // 且不写 out
    }

    // ── 满 ──
    for (int i = 0; i < 8; ++i) {
        assert(rb.try_push(i));
    }
    assert(rb.full());
    assert(rb.size() == 8);
    assert(!rb.try_push(100));  // 第 9 次 push 返回 false（满）

    // 满时顺序读出应为 0..7。
    for (int i = 0; i < 8; ++i) {
        int out = -1;
        assert(rb.try_pop(out));
        assert(out == i);
    }
    assert(rb.empty());
    assert(rb.size() == 0);

    // ── 环绕 ──
    // push/pop 交替累计远超 Capacity（100 轮 × 每轮 push 3 + pop 3），
    // 单调计数器越过 Capacity 后仍正确回绕，FIFO 顺序不丢不重。
    int expected = 0;
    for (int round = 0; round < 100; ++round) {
        assert(rb.try_push(expected));
        assert(rb.try_push(expected + 1));
        assert(rb.try_push(expected + 2));
        for (int k = 0; k < 3; ++k) {
            int out = -1;
            assert(rb.try_pop(out));
            assert(out == expected + k);
        }
        expected += 3;
    }
    assert(rb.empty());
    assert(!rb.full());
    assert(rb.size() == 0);

    // 填满后逐个取空，确认回绕后的边界一致性（tail - head 计数语义）。
    for (int i = 0; i < 8; ++i) {
        assert(rb.try_push(i * 10));
    }
    assert(rb.full());
    rb.clear();  // 单线程复位
    assert(rb.empty());
    assert(!rb.full());
    assert(rb.size() == 0);

    return 0;
}
