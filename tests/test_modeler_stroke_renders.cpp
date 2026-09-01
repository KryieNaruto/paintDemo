// Bug #3 回归测试（C API + Vulkan 离屏）：Stroke modeler 激活后必须能渲染出可用笔画。
//
// 复现（修复前）：任意 modeler 参数（如 KALMAN_PROCESS_NOISE=0.01）激活预测器后，未调
//   `dgcSetFixedTime` 时 `FixedTimeStepper::next()` 恒返回 0 → 所有点 dt=0 →
//   `PositionModeler::Update` 跳过弹簧积分、把每个点钉死在首点 → 笔画塌缩成点（墨迹≈一个 dab）。
// 红：激活 modeler 的墨迹远低于基线。
// 绿（Fix A + Fix B）：墨迹 ≥ 无 modeler 基线 50%，横向跨度 ≥ 输入跨度 80%，纵向极差 ≥
//   输入极差 50%（既防塌缩成点，也防抹平成杠）。
#include "dgc_paint_c_api.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
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
// 输入波浪：x 从 20 到 340（输入横向跨度 320），y = 80 + 20*sin(2π * i/47)
// （1 个周期，输入纵向极差 40）。低频波让 wobble 低通（40ms）衰减有限，Fix B 后弹簧能跟上。
constexpr float kX0 = 20.0f;
constexpr float kXSpan = 320.0f;
constexpr float kY0 = 80.0f;
constexpr float kYAmp = 20.0f;
constexpr float kInputRangeY = 2.0f * kYAmp;  // 40

struct WaveStats {
    std::size_t ink = 0;   // 暗像素（r/g/b < 200）
    int min_x = kW, max_x = -1;
    int min_y = kH, max_y = -1;
    int span_x() const { return max_x >= 0 ? (max_x - min_x + 1) : 0; }
    int range_y() const { return max_y >= 0 ? (max_y - min_y + 1) : 0; }
};

// 画 48 点波浪笔画（不调 dgcSetFixedTime，走默认时间路径）。activateModeler 控制是否
// 先设 KALMAN_PROCESS_NOISE=0.01 惰性激活预测器。填充 buf 并统计墨迹，同时导出 PNG。
WaveStats RenderWave(bool activateModeler, std::vector<std::uint8_t>& buf) {
    WaveStats st;
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return st;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) {
        return st;
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    if (activateModeler) {
        if (dgcSetBrushSetting(ctx.get(), DGC_DEFAULT_BRUSH,
                               DGC_SETTING_KALMAN_PROCESS_NOISE, 0.01) != DGC_OK) {
            return st;
        }
    }
    dgcBeginStroke(ctx.get(), kX0, kY0, 0.5f, 0.0f, 0.0f);
    for (int i = 1; i < kPoints; ++i) {
        const float x = kX0 + kXSpan * (float(i) / (float)(kPoints - 1));
        const float y = kY0 + kYAmp * float(std::sin(2.0 * M_PI * (double(i) / (double)(kPoints - 1))));
        dgcStrokeTo(ctx.get(), x, y, 0.5f, 0.0f, 0.0f, 0);
    }
    dgcEndStroke(ctx.get());
    dgcFlush(ctx.get());

    buf.assign((std::size_t)kW * kH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) {
        return st;
    }
    dgcExportPNG(ctx.get(), activateModeler ? "bugfix_modeler_on.png" : "bugfix_modeler_off.png");
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::uint8_t* p = &buf[((std::size_t)y * kW + (std::size_t)x) * 4];
            if (p[0] < 200 && p[1] < 200 && p[2] < 200) {
                ++st.ink;
                if (x < st.min_x) st.min_x = x;
                if (x > st.max_x) st.max_x = x;
                if (y < st.min_y) st.min_y = y;
                if (y > st.max_y) st.max_y = y;
            }
        }
    }
    return st;
}

}  // namespace

int main() {
    std::vector<std::uint8_t> bufBase, bufModel;
    const WaveStats base = RenderWave(false, bufBase);   // 无 modeler 基线（passthrough）
    const WaveStats mod = RenderWave(true, bufModel);    // 激活 modeler

    std::fprintf(stderr,
                 "[test_modeler_stroke_renders] baseline ink=%zu span_x=%d range_y=%d | "
                 "modeler ink=%zu span_x=%d range_y=%d\n",
                 base.ink, base.span_x(), base.range_y(),
                 mod.ink, mod.span_x(), mod.range_y());

    // 红（修复前）：modeler 墨迹 ≈ 一个 dab，远低于基线 50%。
    CHECK(mod.ink >= base.ink / 2, "modeler ink >= 50% of no-modeler baseline");
    // 绿：保持输入笔迹形态——横向跨度 ≥ 输入跨度的 80%。
    CHECK(mod.span_x() >= (int)(kXSpan * 0.8f), "modeler horizontal span >= 80% of input span");
    // 绿：纵向极差 ≥ 输入纵向极差的 50%（防抹平成水平杠）。
    CHECK(mod.range_y() >= (int)(kInputRangeY * 0.5f), "modeler vertical range >= 50% of input range");

    if (failures == 0) {
        std::fprintf(stderr, "[test_modeler_stroke_renders] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
