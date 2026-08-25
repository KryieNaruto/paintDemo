// B1-7 确定性机制单测：同 seed 同序列 / 不同 seed 异 / ReplayRandom 回放 /
// 固定时间步进 / C API setter 冒烟。机制正确性直接测 core/determinism.h。
//
// B5-3 扩展：端到端「同脚本同 seed 像素级一致」证明（§阶段7）+ golden PNG 逐字节 diff（§4.0.3）。
// 只 #include 公开头 dgc_paint_c_api.h 驱动渲染（Pimpl 边界），不 include render/kernels 内部头；
// DgcContext* 生命周期一律 CtxGuard（unique_ptr + dgcDestroy deleter）RAII 配对，无裸 new/delete。
#include "core/determinism.h"
#include "dgc_paint_c_api.h"

#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond, name) do { if (!(cond)) { std::fprintf(stderr, "FAIL: %s\n", name); ++failures; } } while (0)

namespace {

// DgcContext RAII 守卫（同 cli/script_runner.cpp 先例）：异常/提前返回均自动 dgcDestroy。
using CtxGuard = std::unique_ptr<DgcContext, decltype(&dgcDestroy)>;

// 规范确定性笔画序列（与 tests/fixtures/determinism_script.json 一致）：一条对角三折线。
void runDeterministicStroke(DgcContext* ctx) {
    dgcBeginStroke(ctx, 16.0f, 16.0f, 0.5f, 0.0f, 0.0f);
    dgcStrokeTo(ctx, 64.0f, 64.0f, 0.7f, 0.2f, -0.1f, 0);
    dgcStrokeTo(ctx, 112.0f, 112.0f, 0.9f, 0.3f, -0.2f, 0);
    dgcEndStroke(ctx);
}

// 端到端渲染一次，读回 RGBA 像素。返回是否成功（offscreen/readback 均 DGC_OK）。
bool renderOnce(std::vector<uint8_t>& out, uint64_t seed, double fixedTime) {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) return false;
    if (dgcSetOffscreenSurface(ctx.get(), 128, 128) != DGC_OK) return false;
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    dgcSetRandomSeed(ctx.get(), seed);
    dgcSetFixedTime(ctx.get(), fixedTime);
    runDeterministicStroke(ctx.get());
    dgcFlush(ctx.get());
    out.assign((size_t)128 * 128 * 4, 0);
    return dgcReadbackPixels(ctx.get(), out.data()) == DGC_OK;
}

// 端到端渲染一次并导出 PNG。返回是否成功。
bool renderAndExportOnce(const std::string& path, uint64_t seed, double fixedTime) {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) return false;
    if (dgcSetOffscreenSurface(ctx.get(), 128, 128) != DGC_OK) return false;
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    dgcSetRandomSeed(ctx.get(), seed);
    dgcSetFixedTime(ctx.get(), fixedTime);
    runDeterministicStroke(ctx.get());
    dgcFlush(ctx.get());
    return dgcExportPNG(ctx.get(), path.c_str()) == DGC_OK;
}

// 离屏能力探测（Null 后端 → false）。
bool offscreenAvailable() {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    return ctx && dgcSetOffscreenSurface(ctx.get(), 64, 64) == DGC_OK;
}

// 读整个文件为字节串；失败返回空。
std::vector<uint8_t> readFileBytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)),
                                std::istreambuf_iterator<char>());
}

}  // namespace

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
    {
        CtxGuard ctx(dgcCreate(), &dgcDestroy);
        CHECK(ctx != nullptr, "dgcCreate non-null");
        CHECK(dgcSetRandomSeed(ctx.get(), 42) == DGC_OK, "setRandomSeed OK");
        CHECK(dgcSetFixedTime(ctx.get(), 1000000.0) == DGC_OK, "setFixedTime OK");
    }

    // ── B5-3 端到端确定性 + golden（离屏可用才跑，否则 SKIP）──
    if (offscreenAvailable()) {
        // 1) 同 seed 两次 readback 像素级一致（§阶段7 主验收）。
        std::vector<uint8_t> bufA, bufB;
        const bool ra = renderOnce(bufA, 42, 1000000.0);
        const bool rb = renderOnce(bufB, 42, 1000000.0);
        CHECK(ra && rb, "determinism render twice succeeds");
        if (ra && rb) {
            CHECK(bufA.size() == bufB.size() &&
                  std::memcmp(bufA.data(), bufB.data(), bufA.size()) == 0,
                  "same seed -> same pixels (memcmp==0)");

            // 2) 不同 seed 观察值：不 CHECK（风险 R2——默认笔刷 offset_by_random=0、
            //    random_input 返回值丢弃，seed 当前不改变像素），仅记录供人工观察。
            std::vector<uint8_t> bufC;
            if (renderOnce(bufC, 43, 1000000.0)) {
                const bool sameAsA = (bufA.size() == bufC.size() &&
                                      std::memcmp(bufA.data(), bufC.data(), bufA.size()) == 0);
                std::fprintf(stderr,
                             "[test_determinism] seed 42 vs 43 pixels %s (not asserted, R2)\n",
                             sameAsA ? "identical" : "differ");
            }
        }

        // 3) golden PNG 逐字节 diff（§4.0.3）：两次导出 PNG 逐字节一致 + 对 golden 全文件 memcmp。
        const std::string pngA = "/tmp/b5_3_det_a.png";
        const std::string pngB = "/tmp/b5_3_det_b.png";
        ::remove(pngA.c_str());
        ::remove(pngB.c_str());
        CHECK(renderAndExportOnce(pngA, 42, 1000000.0), "export PNG A succeeds");
        CHECK(renderAndExportOnce(pngB, 42, 1000000.0), "export PNG B succeeds");
        const std::vector<uint8_t> bytesA = readFileBytes(pngA);
        const std::vector<uint8_t> bytesB = readFileBytes(pngB);
        CHECK(!bytesA.empty() && bytesA == bytesB, "two export PNGs byte-identical");

        const std::string golden = std::string(DGC_FIXTURE_DIR) + "/golden_determinism.png";
        const std::vector<uint8_t> goldenBytes = readFileBytes(golden);
        if (goldenBytes.empty()) {
            std::fprintf(stderr,
                         "[test_determinism] SKIP golden byte-diff: fixture 缺失 %s "
                         "(先跑 tests/fixtures/regenerate_golden.sh)\n", golden.c_str());
        } else {
            CHECK(bytesA == goldenBytes, "export PNG matches golden byte-for-byte");
        }
        ::remove(pngA.c_str());
        ::remove(pngB.c_str());
    } else {
        std::fprintf(stderr,
                     "[test_determinism] SKIP offscreen/golden assertions (offscreen unsupported)\n");
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_determinism] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
