// Bug #3 回归测试（C API + Vulkan 离屏）：modeler 参数必须可观测地改变输出。
// PREDICTION_INTERVAL_MS 极大（100ms，每真实点外推 ~18 个预测点）vs 极小（1ms，每真实点
// 外推 1 个预测点）→ 两次输出 PNG 必须不同。
// 红（修复前）：全零时间戳把整条笔画塌缩成首点一个点，两参数下的输出逐字节相同。
#include "dgc_paint_c_api.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>

static int failures = 0;
#define CHECK(cond, name)                                                  \
    do {                                                                   \
        if (!(cond)) {                                                     \
            std::fprintf(stderr, "FAIL: %s\n", name);                      \
            ++failures;                                                    \
        }                                                                  \
    } while (0)

using CtxGuard = std::unique_ptr<DgcContext, decltype(&dgcDestroy)>;

namespace {

constexpr int kW = 400;
constexpr int kH = 160;
constexpr int kPoints = 48;

// 画同一波浪笔画，先设 PREDICTION_INTERVAL_MS=interval 激活 modeler，读回 RGBA 并导出 PNG。
bool RenderWithPrediction(double intervalMs, std::vector<std::uint8_t>& buf, const char* png) {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return false;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) {
        return false;
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    if (dgcSetBrushSetting(ctx.get(), DGC_DEFAULT_BRUSH,
                           DGC_SETTING_PREDICTION_INTERVAL_MS, intervalMs) != DGC_OK) {
        return false;
    }
    dgcBeginStroke(ctx.get(), 20.0f, 80.0f, 0.5f, 0.0f, 0.0f);
    for (int i = 1; i < kPoints; ++i) {
        const float x = 20.0f + 320.0f * (float(i) / (float)(kPoints - 1));
        const float y = 80.0f + 20.0f * float(std::sin(2.0 * M_PI * (double(i) / (double)(kPoints - 1))));
        dgcStrokeTo(ctx.get(), x, y, 0.5f, 0.0f, 0.0f, 0);
    }
    // A8-2：预测点不再永久合墨（endStroke 清 tip），故 PREDICTION_INTERVAL_MS 只改变
    // 「笔画进行中」的湿尖（tip）显示、不改变落笔后的永久墨。这里**不 endStroke**，
    // 让预测尖保留在读回里，验证 interval 仍可观测地改变输出（长 interval → 更长的
    // tip 墨迹）。落笔后 tip 清空的确定性由 test_wet_tip 的验收 B/C 覆盖。
    dgcFlush(ctx.get());
    buf.assign((std::size_t)kW * kH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) {
        return false;
    }
    dgcExportPNG(ctx.get(), png);
    return true;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> bufLarge, bufSmall;
    const bool okLarge = RenderWithPrediction(100.0, bufLarge, "bugfix_modeler_interval_large.png");
    const bool okSmall = RenderWithPrediction(1.0, bufSmall, "bugfix_modeler_interval_small.png");
    CHECK(okLarge && okSmall, "both prediction-interval renders succeed");

    std::size_t diff = 0;
    if (bufLarge.size() == bufSmall.size()) {
        for (std::size_t i = 0; i < bufLarge.size(); ++i) {
            if (bufLarge[i] != bufSmall[i]) {
                ++diff;
            }
        }
    }
    std::fprintf(stderr,
                 "[test_modeler_param_changes_output] interval=100ms vs 1ms: %zu differing bytes\n",
                 diff);
    // 红（修复前）：两输出逐字节相同（塌缩）→ diff==0；绿：diff>0（参数可观测）。
    CHECK(diff > 0, "PREDICTION_INTERVAL_MS large vs small produces different output");

    if (failures == 0) {
        std::fprintf(stderr, "[test_modeler_param_changes_output] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
