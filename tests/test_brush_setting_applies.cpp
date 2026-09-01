// Bug #1 回归测试（C API + Vulkan 离屏）：笔刷内核基础参数 settingId 0-2 必须真实作用于渲染。
//
// 红（修复前）：`dgcSetBrushSetting(DGC_SETTING_RADIUS=40)` 只写 `Impl::brush_settings`（死存储，
//             无任何读取点），内核渲染仍用 createBrush(BrushParams{}) 的默认参数 → 两张画布
//             逐字节相同（参数被旁路）。
// 绿（修复后）：0-3 分支经 `IPaintKernel::setBrushSetting` → `Brush::setBase` 落地内核，半径 40
//             的墨迹显著更大（>1.3×）且横向跨度显著增大；hardness/opacity 也改变输出。
#include "dgc_paint_c_api.h"

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

constexpr int kW = 320;
constexpr int kH = 128;

struct InkStats {
    std::size_t dark_pixels = 0;  // r/g/b 均 < 200 的「墨迹像素」（含浅灰，opacity 0.1 也计入）
    std::size_t black_pixels = 0; // r/g/b 均 < 100 的「强墨迹像素」（仅近不透明笔迹达到）
    int min_x = kW, max_x = -1;
    int span_x() const { return max_x >= 0 ? (max_x - min_x + 1) : 0; }
};

// 渲染一条固定水平线笔画（y=64，x 从 40 到 280），可经 dgcSetBrushSetting 设置一个
// 笔刷内核参数（settingId < 0 表示不设置）。填充 buf 并统计墨迹，同时导出 PNG 供人工对比。
InkStats RenderLine(int settingId, double value, std::vector<std::uint8_t>& buf) {
    InkStats st;
    CtxGuard ctx(dgcCreate(), &dgcDestroy);
    if (!ctx) {
        return st;
    }
    if (dgcSetOffscreenSurface(ctx.get(), kW, kH) != DGC_OK) {
        return st;
    }
    dgcClear(ctx.get(), 1.0f, 1.0f, 1.0f, 1.0f);
    if (settingId >= 0) {
        if (dgcSetBrushSetting(ctx.get(), DGC_DEFAULT_BRUSH, settingId, value) != DGC_OK) {
            return st;
        }
    }
    dgcBeginStroke(ctx.get(), 40.0f, 64.0f, 0.5f, 0.0f, 0.0f);
    for (int x = 48; x <= 280; x += 8) {
        dgcStrokeTo(ctx.get(), (float)x, 64.0f, 0.5f, 0.0f, 0.0f, 0);
    }
    dgcEndStroke(ctx.get());
    dgcFlush(ctx.get());  // drain 屏障：入队 → 三线程合成完毕 → 读回确定性

    buf.assign((std::size_t)kW * kH * 4, 0);
    if (dgcReadbackPixels(ctx.get(), buf.data()) != DGC_OK) {
        return st;
    }
    switch (settingId) {
        case DGC_SETTING_RADIUS:  dgcExportPNG(ctx.get(), "bugfix_brush_radius40.png"); break;
        case DGC_SETTING_HARDNESS: dgcExportPNG(ctx.get(), "bugfix_brush_hardness.png"); break;
        case DGC_SETTING_OPACITY: dgcExportPNG(ctx.get(), "bugfix_brush_opacity.png"); break;
        default:                   dgcExportPNG(ctx.get(), "bugfix_brush_default.png"); break;
    }
    for (int y = 0; y < kH; ++y) {
        for (int x = 0; x < kW; ++x) {
            const std::uint8_t* p = &buf[((std::size_t)y * kW + (std::size_t)x) * 4];
            if (p[0] < 200 && p[1] < 200 && p[2] < 200) {
                ++st.dark_pixels;
                if (x < st.min_x) st.min_x = x;
                if (x > st.max_x) st.max_x = x;
            }
            if (p[0] < 100 && p[1] < 100 && p[2] < 100) {
                ++st.black_pixels;
            }
        }
    }
    return st;
}

}  // namespace

int main() {
    // ── 1) radius=40 vs 默认：墨迹显著增大 + 横向跨度显著增大 ──
    std::vector<std::uint8_t> bufDefault, bufR40;
    const InkStats def = RenderLine(-1, 0.0, bufDefault);
    const InkStats r40 = RenderLine(DGC_SETTING_RADIUS, 40.0, bufR40);
    std::fprintf(stderr,
                 "[test_brush_setting_applies] default ink=%zu span_x=%d | radius40 ink=%zu span_x=%d\n",
                 def.dark_pixels, def.span_x(), r40.dark_pixels, r40.span_x());

    // 红判据：修复前两画布逐字节相同（参数被旁路）；绿判据：不同且半径更大。
    CHECK(bufR40.size() == bufDefault.size() &&
              std::memcmp(bufR40.data(), bufDefault.data(), bufDefault.size()) != 0,
          "radius40 output differs byte-wise from default");
    CHECK(r40.dark_pixels > def.dark_pixels * 1.3, "radius40 ink > 1.3x default ink");
    CHECK(r40.span_x() > def.span_x(), "radius40 horizontal span > default span");

    // ── 2) hardness=1.0 改变输出（非逐字节相同）──
    std::vector<std::uint8_t> bufHard;
    const InkStats hard = RenderLine(DGC_SETTING_HARDNESS, 1.0, bufHard);
    CHECK(bufHard.size() == bufDefault.size() &&
              std::memcmp(bufHard.data(), bufDefault.data(), bufDefault.size()) != 0,
          "hardness=1.0 changes output");

    // ── 3) opacity=0.1 使「强墨迹」（近黑像素）大幅减少（变淡）──
    // 用 black_pixels（<100）而非 dark_pixels（<200）：opacity 0.1 的 dab 叠加后中心仍可
    // 到 ~167，dark 阈值区分度差；近黑像素只有近不透明笔迹才达到，区分度大。
    std::vector<std::uint8_t> bufOp;
    const InkStats op = RenderLine(DGC_SETTING_OPACITY, 0.1, bufOp);
    std::fprintf(stderr,
                 "[test_brush_setting_applies] hardness ink=%zu | opacity ink=%zu black=%zu\n",
                 hard.dark_pixels, op.dark_pixels, op.black_pixels);
    CHECK(bufOp.size() == bufDefault.size() &&
              std::memcmp(bufOp.data(), bufDefault.data(), bufDefault.size()) != 0,
          "opacity=0.1 changes output");
    CHECK(op.black_pixels < def.black_pixels / 2, "opacity=0.1 near-black ink < half of default");

    if (failures == 0) {
        std::fprintf(stderr, "[test_brush_setting_applies] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
