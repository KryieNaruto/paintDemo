// Bug #3 回归测试（C API + Vulkan 离屏）：modeler 路径确定性——同输入同参数两次运行，
// 输出画布逐字节一致。
// 注：不要求「与修复前一致」——Fix B 上调弹簧默认值必然改变 modeler 输出，属预期修复，
//     不是回归；本测试只锁定「可复现」（同参同输入 → 同输出），Fix A 的固定默认时间步
//     保证该确定性成立。
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

// 同输入同参数渲染一次（含固定 seed 0 与 modeler 激活）。
bool RenderOnce(std::vector<std::uint8_t>& buf, const char* png) {
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return false;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) {
        return false;
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    dgcSetRandomSeed(ctx.get(), 0);
    dgcSetBrushSetting(ctx.get(), DGC_DEFAULT_BRUSH,
                       DGC_SETTING_KALMAN_PROCESS_NOISE, 0.01);
    dgcBeginStroke(ctx.get(), 20.0f, 80.0f, 0.5f, 0.0f, 0.0f);
    for (int i = 1; i < kPoints; ++i) {
        const float x = 20.0f + 320.0f * (float(i) / (float)(kPoints - 1));
        const float y = 80.0f + 20.0f * float(std::sin(2.0 * M_PI * (double(i) / (double)(kPoints - 1))));
        dgcStrokeTo(ctx.get(), x, y, 0.5f, 0.0f, 0.0f, 0);
    }
    dgcEndStroke(ctx.get());
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
    std::vector<std::uint8_t> a, b;
    const bool okA = RenderOnce(a, "bugfix_modeler_det_a.png");
    const bool okB = RenderOnce(b, "bugfix_modeler_det_b.png");
    CHECK(okA && okB, "both deterministic renders succeed");
    CHECK(a.size() == b.size() &&
              std::memcmp(a.data(), b.data(), a.size()) == 0,
          "same input+params renders byte-identical");

    if (failures == 0) {
        std::fprintf(stderr, "[test_modeler_deterministic] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
