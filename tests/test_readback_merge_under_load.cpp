// A8-3 §5.2 host 回归测试：候选①（4a 合并命中率在高频读回下是否退化）的永久护栏。
//
// 背景（A8-3 计划 §3.1/§3.4/§5.2）：任务行怀疑「readback 去 16ms 节流」需要先证实 4a
// （合并 composite 与读回刷新为一次 GPU 提交）在真机连续快画 + 高频 dgcReadbackPixels
// 交织场景下依然生效（不会退化成「每次读回各自触发一次独立 GPU 提交」）。
// `test_snapshot_refresh_throttle.cpp` 覆盖了「连续 stroke、完全不中途读回」的场景，
// 特意跳过了「中途高频读回」这一组合——本测试补上这一部分。
//
// 断言的布尔事实（不依赖具体耗时数字，只依赖代码路径）：
//   dgcTestSubmitAndWaitCount(ctx) - dgcTestCompositeCount(ctx) 的增量应在整个连续绘制
//   过程中保持有界常数，不随「中途读回调用次数」线性增长。
// 若 4a 合并退化为「每次 composite 触发的刷新都独立提交」，随着读回次数增多，
// submitAndWaitCount 会相对 compositeCount 额外增长（因为更多次 composite 会命中一个
// 待处理的 flush 请求，若合并失效则每次都多付一次独立提交）——这正是候选①成立时应该
// 观测到的信号；本测试用大量交织读回（每 8 点一次，覆盖率远高于真机 16ms/4ms 节流下
// 单笔画的读回密度）放大这个信号，若合并健康，submitAndWaitCount 应仍然贴着
// compositeCount + 小常数，不随读回次数增长。
//
// 采用「每 N 个笔点」而非 wall-clock sleep 的节流方式触发读回：这是纯代码路径问题
// （读回是否触发额外独立提交），不是时序问题，用确定性的点数节流即可复现，避免
// wall-clock 抖动导致的 flaky。
#include "dgc_paint_c_api.h"

#include <cstdint>
#include <cstdio>
#include <thread>
#include <chrono>
#include <vector>

#ifdef DGCPAIN_TEST_HOOKS
// 测试访问器不在公开头（dgc_paint_c_api.h）中（仅测试编译进库），此处自行声明，
// 与 test_snapshot_refresh_throttle.cpp 完全一致的调用方式（复用已有 hook，不新增符号）。
extern "C" std::uint64_t dgcTestCompositeCount(DgcContext* ctx);
extern "C" std::uint64_t dgcTestSubmitAndWaitCount(DgcContext* ctx);
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

// 场景与 test_snapshot_refresh_throttle.cpp 一致（24 条水平锯齿线、共 6024 点），
// 但中途每 kReadbackEveryNPoints 个点插入一次 dgcReadbackPixels——模拟消费端在
// ReadbackScheduler 节流值降到 4-8ms 后，单笔画期间读回调用频率大幅提高的场景
// （远高于真机实际密度，用于放大候选①的信号）。
constexpr int kLines = 24;
constexpr int kPointsPerLine = 251;  // (1040-40)/4 + 1
constexpr int kTotalPoints = kLines * kPointsPerLine;
constexpr int kReadbackEveryNPoints = 8;  // 高频读回：整段笔画约 6024/8 ≈ 753 次读回。

// 是否「墨迹像素」：默认白底 (255,255,255)，非近白即墨迹（对齐既有测试）。
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

    std::vector<std::uint8_t> scratchBuf((std::size_t)kW * kH * 4, 0);
    int readbackCalls = 0;

    // ── 连续 stroke（紧密提交，满则 sleep 重试），中途高频交织 dgcReadbackPixels ──
    dgcBeginStroke(ctx, 40.0f, 30.0f, 1.0f, 0.0f, 0.0f);
    int pointIndex = 0;
    for (int l = 0; l < kLines; ++l) {
        const float y = 30.0f + l * 25.0f;
        const bool leftToRight = (l % 2) == 0;
        for (int i = 0; i < kPointsPerLine; ++i, ++pointIndex) {
            const float x = leftToRight ? (40.0f + i * 4.0f) : (1040.0f - i * 4.0f);
            int retries = 0;
            while (dgcStrokeTo(ctx, x, y, 1.0f, 0.0f, 0.0f, 0) != DGC_OK) {
                if (++retries > 200) {
                    std::fprintf(stderr,
                                 "[test_readback_merge_under_load] strokeTo queue-full retry "
                                 "exhausted at point %d\n",
                                 pointIndex);
                    CHECK(false, "strokeTo eventually accepted (no permanent queue-full)");
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }

            if ((pointIndex % kReadbackEveryNPoints) == 0) {
                CHECK(dgcReadbackPixels(ctx, scratchBuf.data()) == DGC_OK,
                      "mid-stroke readback OK");
                ++readbackCalls;
            }
        }
    }
    dgcEndStroke(ctx);

    // ── 收尾：drain 屏障 + 权威读回 + 逐线无孔洞检查（复用既有正确性检查）──
    CHECK(dgcFlush(ctx) == DGC_OK, "flush OK");
    std::vector<std::uint8_t> finalBuf((std::size_t)kW * kH * 4, 0);
    CHECK(dgcReadbackPixels(ctx, finalBuf.data()) == DGC_OK, "final readback OK");
    CHECK(dgcExportPNG(ctx, "readback_merge_under_load.png") == DGC_OK, "export PNG OK");

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
                     "[test_readback_merge_under_load] line y=%d inked=%d/%d (%.1f%%)\n",
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
                 "[test_readback_merge_under_load] totalInk=%zu linesComplete=%d/%d "
                 "points=%d readbackCalls=%d\n",
                 totalInk, linesComplete, kLines, kTotalPoints, readbackCalls);
    CHECK(totalInk > 0, "stroke rendered ink");
    CHECK(linesComplete == kLines,
          "every horizontal line has continuous ink under interleaved high-frequency readback "
          "(no holes introduced by mid-stroke readback)");
    CHECK(readbackCalls > 100, "mid-stroke readback actually exercised at high frequency");

    // ── 候选①核心断言：4a 合并在高频读回交织下不退化 ──
    // 若合并健康：composite 触发的刷新（settle 那部分）仍合并进 composite 自己的提交，
    // 与本测试中途插入了多少次 dgcReadbackPixels（readbackCalls）无关——
    // submitAndWaitCount 应仍然贴着 compositeCount + 小常数（同
    // test_snapshot_refresh_throttle.cpp 的 +4 常数量级：dgcClear 自身两次提交 +
    // dgcFlush 收尾一次 + dgcExportPNG 一次），不随 readbackCalls（~753 次）线性增长。
    // 若合并退化（候选①成立）：每次 composite 命中一个待处理 flush 请求都会额外独立
    // 提交，随着读回次数（进而 requestFlush 次数）增多，submitAndWaitCount 会相对
    // compositeCount 明显多出远大于 4 的差值，且该差值量级会随 readbackCalls 变化——
    // 用一个远大于「小常数」但仍远小于「与 readbackCalls 同量级」的界（compositeCount
    // 的一个固定小倍数 + 常数）来分辨两种情形。
    const std::uint64_t composite = dgcTestCompositeCount(ctx);
    const std::uint64_t submitAndWait = dgcTestSubmitAndWaitCount(ctx);
    std::fprintf(stderr,
                 "[test_readback_merge_under_load] compositeCount=%llu "
                 "submitAndWaitCount=%llu readbackCalls=%d bound=%llu\n",
                 (unsigned long long)composite, (unsigned long long)submitAndWait,
                 readbackCalls, (unsigned long long)(composite + 8));
    CHECK(composite > 0, "composite batches happened (overCap triggered)");
    // 若退化为「每次读回各自独立提交」，submitAndWait 会逼近
    // composite + readbackCalls（~753），远超 composite + 8；用 composite + 8 作为
    // 「合并健康」与「合并退化」之间有巨大安全边际的判据（既有测试用 +4，这里因为
    // 额外交织了读回路径中 dgcClear/收尾不变的独立提交，留 +8 裕量，仍然与
    // readbackCalls 量级（数百次）有数量级差距，不会把候选①成立误判为证伪）。
    CHECK(submitAndWait <= composite + 8,
          "candidate #1 falsified: 4a merge stays effective under interleaved "
          "high-frequency readback (submitAndWaitCount <= compositeCount + 8, "
          "independent of readbackCalls)");

    dgcDestroy(ctx);

    if (failures == 0) {
        std::fprintf(stderr, "[test_readback_merge_under_load] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
