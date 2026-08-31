// Bug #3 回归测试（C API + Vulkan + DGCPAIN_TEST_HOOKS）：快照刷新频率节流。
//
// 根因：VkBackend::CompositeLocked 在**每一次** composite 批提交完成后无条件调用
// RefreshReadbackCacheLocked()（全画布 GPU→CPU 拷贝，Mali 上数 ms）。渲染线程在连续绘制
// 时每 ≤4ms（kMaxBatchDurationMs overCap）composite 一次 → Android 弱 GPU 被 3.1MB 快照
// 拷贝打饱和 → 主线程每帧渲染被拖长 → 60→30 掉帧（PC GPU 拷贝 ~1ms 不掉帧）。
//
// 场景：连续 stroke（24 条横线、共 6024 点，紧密提交、满则 sleep 重试）——连续输入下
// 渲染线程主要靠 overCap（4ms/512-stamp）合批，**不**在中途插入读回（读回会暂停提交循环
// 并注入时序相关的刷新请求，致断言 flaky）。收尾 dgcFlush + 最终 readback + 导出 PNG，
// 断言 24 条线逐条无孔洞（快照完整性）+ 刷新被节流。
//
// 红（修复前）：每次 composite 都无条件刷新 → snapshotRefreshCount ≈ compositeCount
//   （composite ≥ ceil(6024/512)=12 批，+每次 clear 的 1 次 → refresh ≥ 13）→ 断言
//   refresh <= kMaxRefreshBound（=10）失败。
// 绿（修复后）：composite 只在 snapshotRefreshRequested_ 置位时才刷新——刷新仅发生在
//   ① clear ② dgcFlush/dgcReadbackPixels 的 requestFlush 请求（经下一次 composite 消费）
//   ③ dgcFlush 排空后的收尾强制刷新（Engine::flush → backend_->flushReadbackCache()）。
//   overCap 自动合批的 composite 不再刷新 → refresh 与 composite 数量解耦（确定性），
//   本场景无中途读回 → refresh = clear(1) + drain 收尾(1) = 2。断言 refresh <= 10
//   （修复前 ≥ 13 必然超界；修复后 ≤ 2 必然通过，不随调度波动）。
//
// 计数方式：compositeCount = VkBackend::CompositeLocked 通过空检查的实际批提交次数；
// snapshotRefreshCount = 每次实际 RefreshReadbackCacheLocked（CompositeLocked 末尾 +
// ClearCanvasLocked）调用。经 C API 测试访问器 dgcTestSnapshotRefreshCount/
// dgcTestCompositeCount（仅 TEST_HOOKS 编译）读出。用独立 composite 计数而非
// dispatchCount 换算：dispatchCount 按 dab 计，而批边界由 renderLoop 的 4ms 攒批上限
// （时间/点数动态）决定，无法从 dispatch 反推批数；compositeCount 才是「composite 次数」
// 单位。
#include "dgc_paint_c_api.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

#ifdef DGCPAIN_TEST_HOOKS
// 测试访问器不在公开头（dgc_paint_c_api.h）中（仅测试编译进库），此处自行声明。
extern "C" std::uint64_t dgcTestSnapshotRefreshCount(DgcContext* ctx);
extern "C" std::uint64_t dgcTestCompositeCount(DgcContext* ctx);
#endif

static int failures = 0;
#define CHECK(cond, name)                            \
    do {                                              \
        if (!(cond)) {                                \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                                \
        }                                              \
    } while (0)

namespace {

constexpr int kW = 1080;
constexpr int kH = 720;

// 24 条水平线锯齿路径：y = 30 + l*25（30..605），x 在 40..1040 间以 4px 步进往返。
// 半径默认 10、间距 4px → 每条线是实心墨带，逐线检查无孔洞。
constexpr int kLines = 24;
constexpr int kPointsPerLine = 251;                 // (1040-40)/4 + 1
constexpr int kTotalPoints = kLines * kPointsPerLine;
// 刷新上界：clear(1) + 队列排空结算（弱 GPU 下渲染线程落后，实测 ≤4）+ 收尾 drain(1)，
// 留 4 裕量 → 10。修复前 refresh ≈ composite ≥ 13 必然超界（红）。修复后 refresh ∈ [2,6]
// 必然通过（绿），且不与 composite 数量挂钩（composite 随调度在 12~40 波动）。
constexpr int kMaxRefreshBound = 10;

// 是否「墨迹像素」：默认白底 (255,255,255)，非近白即墨迹（对齐 test_brush_setting_applies）。
bool IsInk(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return r < 200 && g < 200 && b < 200;
}

}  // namespace

int main() {
    DgcContext* ctx = dgcCreate();
    CHECK(ctx != nullptr, "dgcCreate non-null");
    if (ctx == nullptr) {
        return 1;
    }
    CHECK(dgcSetOffscreenSurface(ctx, kW, kH) == DGC_OK, "offscreen init OK");
    CHECK(dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f) == DGC_OK, "clear white");

    // ── 连续 stroke（紧密提交，满则 sleep 重试；不中途读回）──
    dgcBeginStroke(ctx, 40.0f, 30.0f, 1.0f, 0.0f, 0.0f);
    int pointIndex = 0;
    for (int l = 0; l < kLines; ++l) {
        const float y = 30.0f + l * 25.0f;
        const bool leftToRight = (l % 2) == 0;
        for (int i = 0; i < kPointsPerLine; ++i, ++pointIndex) {
            const float x = leftToRight ? (40.0f + i * 4.0f) : (1040.0f - i * 4.0f);
            // 队列满（提交快于引擎消费）时 sleep 重试，保证连续不间断输入、笔点不丢。
            int retries = 0;
            while (dgcStrokeTo(ctx, x, y, 1.0f, 0.0f, 0.0f, 0) != DGC_OK) {
                if (++retries > 200) {
                    std::fprintf(stderr, "[test_snapshot_refresh_throttle] strokeTo queue-full retry "
                                         "exhausted at point %d\n", pointIndex);
                    CHECK(false, "strokeTo eventually accepted (no permanent queue-full)");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
    }
    dgcEndStroke(ctx);

    // ── 收尾：drain 屏障 + 权威读回 + 逐线无孔洞检查 ──
    CHECK(dgcFlush(ctx) == DGC_OK, "flush OK");
    std::vector<std::uint8_t> finalBuf((std::size_t)kW * kH * 4, 0);
    CHECK(dgcReadbackPixels(ctx, finalBuf.data()) == DGC_OK, "final readback OK");
    CHECK(dgcExportPNG(ctx, "bugfix_snapshot_throttle.png") == DGC_OK, "export PNG OK");

    // 逐线墨带完整性：每条线在其 y 带（y0-2..y0+2）内、x∈[80,1000] 上被实心覆盖
    // （无缺失 dab 造成的孔洞）。每条线的被覆盖 x 比例 ≥ 85%。
    std::size_t totalInk = 0;
    int linesComplete = 0;
    for (int l = 0; l < kLines; ++l) {
        const int y0 = 30 + l * 25;
        int inkedColumns = 0;
        int checkedColumns = 0;
        for (int x = 80; x <= 1000; x += 2) {
            ++checkedColumns;
            bool colInk = false;
            for (int dy = -2; dy <= 2; ++dy) {
                const std::uint8_t* p =
                    &finalBuf[((std::size_t)(y0 + dy) * kW + (std::size_t)x) * 4];
                if (IsInk(p[0], p[1], p[2])) {
                    colInk = true;
                    break;
                }
            }
            if (colInk) {
                ++inkedColumns;
            }
        }
        const double frac = (double)inkedColumns / (double)checkedColumns;
        if (frac >= 0.85) {
            ++linesComplete;
        }
        std::fprintf(stderr,
                     "[test_snapshot_refresh_throttle] line y=%d inked=%d/%d (%.1f%%)\n",
                     y0, inkedColumns, checkedColumns, frac * 100.0);
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::uint8_t* p = &finalBuf[((std::size_t)y * kW + (std::size_t)x) * 4];
            if (IsInk(p[0], p[1], p[2])) {
                ++totalInk;
            }
        }
    }
    std::fprintf(stderr,
                 "[test_snapshot_refresh_throttle] totalInk=%zu linesComplete=%d/%d points=%d\n",
                 totalInk, linesComplete, kLines, kTotalPoints);
    CHECK(totalInk > 0, "stroke rendered ink");
    CHECK(linesComplete == kLines, "every horizontal line has continuous ink (no holes)");

    // ── 节流断言（本 bug 先红后绿核心）──
    const std::uint64_t refresh = dgcTestSnapshotRefreshCount(ctx);
    const std::uint64_t composite = dgcTestCompositeCount(ctx);
    std::fprintf(stderr,
                 "[test_snapshot_refresh_throttle] snapshotRefreshCount=%llu compositeCount=%llu "
                 "ratio=%.3f\n",
                 (unsigned long long)refresh, (unsigned long long)composite,
                 composite > 0 ? (double)refresh / (double)composite : 0.0);
    CHECK(composite > 0, "composite batches happened (overCap triggered)");
    CHECK(refresh > 0, "snapshot refresh happened (clear + settle + drain)");
    // 确定性上界：刷新只发生在 clear + 队列排空结算 + drain 请求，不随 composite 数量波动。
    // 修复前 refresh ≈ composite ≥ 13 必然超界（红）；修复后 refresh ∈ [2,6] 必然通过（绿）。
    CHECK(refresh <= kMaxRefreshBound,
          "snapshot refresh throttled: refresh <= kMaxRefreshBound "
          "(was ~composite before fix)");

    dgcDestroy(ctx);

    if (failures == 0) {
        std::fprintf(stderr, "[test_snapshot_refresh_throttle] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
