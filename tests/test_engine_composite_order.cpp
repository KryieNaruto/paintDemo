// bugfix-stale-tip 回归（TDD 先红后绿）：CompositeOrdered 必须先 composite 预测批、
// 后 composite 真实批——保证 VkBackend 的读回刷新标志无论被哪次调用消费，看到的
// tipImage 都已反映本批最新预测内容，不是上一批遗留的旧内容。
//
// 白盒直测：不经 Engine 三线程/SPSC 队列，直接用一个记录调用顺序的 fake IRenderBackend
// 调用 CompositeOrdered，验证三种情况（仿 tests/test_flush_throttle_engine.cpp 的
// BatchCountingBackend 写法）：
//   1) 预测批 + 真实批都非空 → 调用顺序必须是 [predicted=true, predicted=false]。
//   2) 只有真实批非空 → 只调用一次，predicted=false。
//   3) 只有预测批非空 → 只调用一次，predicted=true。
//   4) 两者皆空 → 不调用。
#include "core/engine.h"
#include "core/interfaces/i_render_backend.h"

#include <cstdio>
#include <vector>

namespace {

class OrderRecordingBackend : public IRenderBackend {
public:
    void init(PlatformSurface, int, int) override {}
    void resize(int, int) override {}
    void beginFrame() override {}
    void composite(const std::vector<StampData>&, bool predicted) override {
        order.push_back(predicted);
    }
    void clearCanvas(float, float, float, float) override {}
    void present() override {}
    void shutdown() override {}
    void initOffscreen(int, int) override {}
    void readback(void*) override {}
    void exportPNG(const char*) override {}

    std::vector<bool> order;
};

std::vector<StampData> MakeStamps(std::size_t n) {
    std::vector<StampData> v(n);
    for (std::size_t i = 0; i < n; ++i) {
        v[i] = StampData{static_cast<float>(i), 0.0f, 4.0f, 0.9f, 1.0f};
    }
    return v;
}

}  // namespace

static int failures = 0;
#define CHECK(cond, name)                                                 \
    do {                                                                  \
        if (!(cond)) {                                                    \
            std::fprintf(stderr, "FAIL: %s\n", name);                     \
            ++failures;                                                   \
        }                                                                  \
    } while (0)

int main() {
    // 情况 1：预测批 + 真实批都非空 → 预测先、真实后。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred = MakeStamps(3);
        std::vector<StampData> real = MakeStamps(5);
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.size() == 2, "case1: exactly two composite() calls");
        CHECK(backend.order.size() == 2 && backend.order[0] == true,
              "case1: predicted batch composited first");
        CHECK(backend.order.size() == 2 && backend.order[1] == false,
              "case1: real batch composited second");
        CHECK(pred.empty(), "case1: predStamps cleared after composite");
        CHECK(real.empty(), "case1: realStamps cleared after composite");
    }
    // 情况 2：只有真实批非空。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred;
        std::vector<StampData> real = MakeStamps(4);
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.size() == 1 && backend.order[0] == false,
              "case2: only real composited, single call");
    }
    // 情况 3：只有预测批非空。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred = MakeStamps(2);
        std::vector<StampData> real;
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.size() == 1 && backend.order[0] == true,
              "case3: only predicted composited, single call");
    }
    // 情况 4：两者皆空 → 不调用。
    {
        OrderRecordingBackend backend;
        std::vector<StampData> pred;
        std::vector<StampData> real;
        CompositeOrdered(&backend, &pred, &real);
        CHECK(backend.order.empty(), "case4: no composite() call when both empty");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_engine_composite_order] PASS\n");
    } else {
        std::fprintf(stderr, "[test_engine_composite_order] FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
