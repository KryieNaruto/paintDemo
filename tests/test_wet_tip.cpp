// A8-2 验收测试（host 无头）：预测瞬态 wet-tip 层。
//
// 覆盖验收 A-D（eval §5）：
//   A 预测 ON 中间读回含 tip（像素级）——白盒 VkBackend 直测：预测批 composite 后读回
//     在预测尖覆盖区出现墨迹，clearTip 后该区域墨迹消失（确定性，无 engine 异步竞态）。
//   B 落笔后读回 = 纯真实——C API：interval=30（ON）与 interval=0（OFF）同一笔，
//     endStroke + dgcFlush 后读回逐位一致（tip 已清，真实墨逐位相同）。
//   C 导出恒无 tip——C API：ON 与 OFF 导出 PNG 逐字节一致（exportPNG 直读 canvasImage）。
//   D 确定性双跑一致——C API：同 seed 同脚本 ON 两跑读回逐字节一致。
//
// 依赖真实 Vulkan 后端（lavapipe headless）与真实笔刷内核（DGCPAIN_HAVE_BRUSH）。
#include "dgc_paint_c_api.h"
#include "render/vulkan/vk_backend.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

static int failures = 0;
#define CHECK(cond, name)                       \
    do {                                        \
        if (!(cond)) {                          \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                         \
        }                                       \
    } while (0)

namespace {

constexpr int kW = 256;
constexpr int kH = 256;

bool IsInk(std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    return r < 200 && g < 200 && b < 200;
}

struct Sample {
    float x, y;
    std::uint64_t t_us;
};

// 水平直线：x 40→200，60Hz 步进（v≈480px/s），预测 ON 时 tip 沿 +x 外推。
std::vector<Sample> MakeLine() {
    std::vector<Sample> out;
    const std::uint64_t kDtUs = 16667;
    for (int i = 0; i < 21; ++i) {
        out.push_back({40.0f + i * 8.0f, 128.0f, 1000000u + (std::uint64_t)i * kDtUs});
    }
    return out;
}

}  // namespace

// ── 白盒 VkBackend：验收 A（预测批读回含 tip）+ clearTip（落笔丢弃预测尖）──
static int TestBackendTip() {
    int f = 0;
    VkBackend backend;
    backend.initOffscreen(kW, kH);
    backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);  // 白底

    // 真实墨：x 40..100（覆盖到 ~104）；预测尖：x 104..150（覆盖到 ~154）。
    std::vector<StampData> real;
    for (int x = 40; x <= 100; x += 4) {
        real.push_back(StampData{static_cast<float>(x), 128.0f, 4.0f, 0.9f, 1.0f});
    }
    std::vector<StampData> pred;
    for (int x = 104; x <= 150; x += 4) {
        pred.push_back(StampData{static_cast<float>(x), 128.0f, 4.0f, 0.9f, 1.0f});
    }

    backend.requestSnapshotRefresh();
    backend.composite(real, /*predicted=*/false);
    backend.requestSnapshotRefresh();
    backend.composite(pred, /*predicted=*/true);

    std::vector<std::uint8_t> buf1(static_cast<std::size_t>(kW) * kH * 4, 0);
    backend.readback(buf1.data());
    auto inkAt = [&](const std::vector<std::uint8_t>& b, int x, int y) {
        const std::uint8_t* p = &b[((std::size_t)y * kW + (std::size_t)x) * 4];
        return IsInk(p[0], p[1], p[2]);
    };
    // x=130 在预测尖覆盖区（真实墨只到 ~104）→ 读回应含 tip。
    CHECK(inkAt(buf1, 60, 128), "backend: real dab composited to canvas");
    CHECK(inkAt(buf1, 130, 128), "backend: predicted dab merged into readback (tip visible)");

    backend.clearTip();

    std::vector<std::uint8_t> buf2(static_cast<std::size_t>(kW) * kH * 4, 0);
    backend.readback(buf2.data());
    CHECK(inkAt(buf2, 60, 128), "backend: clearTip keeps real ink");
    CHECK(!inkAt(buf2, 130, 128), "backend: clearTip removes predicted tip");

    backend.shutdown();
    return f;
}

// ── C API：验收 B / C / D ──
struct Run {
    bool ok = false;
    std::vector<std::uint8_t> buf;
    std::vector<std::uint8_t> png;
};

static Run RunStroke(float intervalMs, bool endStroke, const char* pngPath) {
    Run r;
    DgcContext* ctx = dgcCreate();
    if (ctx == nullptr) {
        return r;
    }
    if (dgcSetOffscreenSurface(ctx, kW, kH) != DGC_OK) {
        dgcDestroy(ctx);
        return r;
    }
    dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f);
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_RADIUS, 4.0);
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_HARDNESS, 0.9);
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_OPACITY, 1.0);
    dgcSetBrushColor(ctx, DGC_DEFAULT_BRUSH, 0.0f, 0.0f, 0.0f, 1.0f);
    dgcSetRandomSeed(ctx, 42);
    dgcSetBrushSetting(ctx, DGC_DEFAULT_BRUSH, DGC_SETTING_PREDICTION_INTERVAL_MS,
                       static_cast<double>(intervalMs));

    const std::vector<Sample> seq = MakeLine();
    dgcBeginStroke(ctx, seq[0].x, seq[0].y, 0.5f, 0.0f, 0.0f);
    for (std::size_t i = 1; i < seq.size(); ++i) {
        if (dgcStrokeToAt(ctx, seq[i].x, seq[i].y, 0.5f, 0.0f, 0.0f, 0,
                          static_cast<double>(seq[i].t_us)) != DGC_OK) {
            dgcDestroy(ctx);
            return r;
        }
    }
    if (endStroke) {
        dgcEndStroke(ctx);
    }
    if (dgcFlush(ctx) != DGC_OK) {
        dgcDestroy(ctx);
        return r;
    }
    r.buf.assign(static_cast<std::size_t>(kW) * kH * 4, 0);
    if (dgcReadbackPixels(ctx, r.buf.data()) != DGC_OK) {
        dgcDestroy(ctx);
        return r;
    }
    if (pngPath != nullptr) {
        if (dgcExportPNG(ctx, pngPath) != DGC_OK) {
            dgcDestroy(ctx);
            return r;
        }
        std::ifstream f(pngPath, std::ios::binary);
        r.png.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    dgcDestroy(ctx);
    r.ok = true;
    return r;
}

static std::size_t CountInk(const std::vector<std::uint8_t>& b) {
    std::size_t n = 0;
    for (std::size_t i = 0; i + 3 < b.size(); i += 4) {
        if (IsInk(b[i], b[i + 1], b[i + 2])) {
            ++n;
        }
    }
    return n;
}

static int TestCApi() {
    int f = 0;

    // B / C：ON（interval=30）与 OFF（interval=0）同输入，endStroke + flush 后逐位一致。
    Run on = RunStroke(30.0f, /*endStroke=*/true, "wet_tip_on.png");
    Run off = RunStroke(0.0f, /*endStroke=*/true, "wet_tip_off.png");
    CHECK(on.ok && off.ok, "B/C: ON and OFF runs both complete");
    const bool sameBuf =
        on.buf.size() == off.buf.size() &&
        std::memcmp(on.buf.data(), off.buf.data(), on.buf.size()) == 0;
    CHECK(sameBuf, "B: ON endStroke+flush readback == OFF (tip cleared, pure real ink)");
    CHECK(!on.png.empty() && on.png == off.png,
          "C: ON exportPNG == OFF exportPNG (tip never in export)");
    CHECK(CountInk(on.buf) > 0, "B: stroke rendered ink (sanity)");

    // D：同 seed 同脚本 ON 两跑读回逐字节一致。
    Run a = RunStroke(30.0f, /*endStroke=*/true, "wet_tip_det_a.png");
    Run b = RunStroke(30.0f, /*endStroke=*/true, "wet_tip_det_b.png");
    CHECK(a.ok && b.ok, "D: two ON runs both complete");
    const bool sameDet =
        a.buf.size() == b.buf.size() &&
        std::memcmp(a.buf.data(), b.buf.data(), a.buf.size()) == 0;
    CHECK(sameDet, "D: two ON runs byte-identical (determinism)");

    return f;
}

int main() {
    failures += TestBackendTip();
    failures += TestCApi();

    if (failures == 0) {
        std::fprintf(stderr, "[test_wet_tip] PASS\n");
    } else {
        std::fprintf(stderr, "[test_wet_tip] FAILED (%d)\n", failures);
    }
    return failures == 0 ? 0 : 1;
}
