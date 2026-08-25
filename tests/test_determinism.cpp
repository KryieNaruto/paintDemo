// B1-7 确定性机制单测：同 seed 同序列 / 不同 seed 异 / ReplayRandom 回放 /
// 固定时间步进 / C API setter 冒烟。机制正确性直接测 core/determinism.h，
// 端到端「同脚本同 seed 像素级一致」归 B5-3（golden PNG）。
#include "core/determinism.h"
#include "dgc_paint_c_api.h"

#include <cstdint>
#include <cmath>
#include <vector>
#include <cstdio>

static int failures = 0;
#define CHECK(cond, name) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; } } while (0)

int main() {
    // a) 同 seed 同序列
    Mt19937Random a(42), b(42);
    for (int i = 0; i < 1000; ++i) {
        CHECK(a.nextUniform() == b.nextUniform(), "same seed -> same uniform");
        CHECK(a.nextGauss()   == b.nextGauss(),   "same seed -> same gauss");
    }

    // b) 不同 seed 至少一处异：用两个全新实例 d(42)/e(43) 逐值比较，
    //    不复用已在 (a) 推进过的 a/b（plan-review 建议 1）。
    Mt19937Random d(42), e(43);
    bool diff = false;
    for (int i = 0; i < 1000 && !diff; ++i) {
        diff = (d.nextUniform() != e.nextUniform());
    }
    CHECK(diff, "different seed -> different sequence");

    // c) ReplayRandom 回放给定序列逐值相等
    std::vector<double> seq{0.1, 0.2, 0.3, 0.4};
    ReplayRandom r(seq);
    CHECK(r.nextUniform() == 0.1 && r.nextGauss() == 0.2 && r.nextUniform() == 0.3,
          "replay order");

    // d) 固定时间步进：override 时 0, step, 2*step, …；非 override 恒 0
    FixedTimeStepper s;
    s.beginStroke(1000.0, true);
    CHECK(s.next() == 0 && s.next() == 1000 && s.next() == 2000, "fixed step");
    FixedTimeStepper s2;
    s2.beginStroke(1000.0, false);
    CHECK(s2.next() == 0 && s2.next() == 0, "no override -> 0");

    // e) C API setter 冒烟（机制已在 a-d 覆盖，这里只验返回码）
    DgcContext* ctx = dgcCreate();
    CHECK(ctx != nullptr, "dgcCreate non-null");
    CHECK(dgcSetRandomSeed(ctx, 42) == DGC_OK, "setRandomSeed OK");
    CHECK(dgcSetFixedTime(ctx, 1000000.0) == DGC_OK, "setFixedTime OK");
    dgcDestroy(ctx);

    return failures == 0 ? 0 : 1;
}
