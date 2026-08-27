// B3-1 host ctest（Vulkan 开）：离屏集成验证。
// 1) C API 端到端：dgcCreate → offscreen → beginStroke → strokeTo×N → endStroke → flush →
//    readback，断言画布相对白底出现可见笔刷痕迹（验收 4「送入 vk_composite 可画」）。
// 2) 直接 VkBackend + 红色 StampData：断言逐 dab 颜色烘焙生效（验收 3「颜色调制可见」）。
#include <cstdint>
#include <cstdio>
#include <vector>

#include "dgc_paint_c_api.h"
#include "render/vulkan/vk_backend.h"

static int failures = 0;
#define CHECK(cond, name)                       \
    do {                                        \
        if (!(cond)) {                          \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                         \
        }                                       \
    } while (0)

int main() {
    constexpr int kW = 64;
    constexpr int kH = 64;

    // ── 1) C API 端到端 ──
    {
        DgcContext* ctx = dgcCreate();
        CHECK(ctx != nullptr, "dgcCreate non-null");
        if (ctx != nullptr) {
            CHECK(dgcSetOffscreenSurface(ctx, kW, kH) == DGC_OK, "offscreen init OK");
            dgcClear(ctx, 1.0f, 1.0f, 1.0f, 1.0f);  // 清成不透明白底

            // 水平直线 y=32，x 从 8 到 56（step 4px）。
            dgcBeginStroke(ctx, 8.0f, 32.0f, 0.5f, 0.0f, 0.0f);
            for (int x = 12; x <= 56; x += 4) {
                dgcStrokeTo(ctx, (float)x, 32.0f, 0.5f, 0.0f, 0.0f, 0);
            }
            dgcEndStroke(ctx);
            dgcFlush(ctx);  // 等三线程把全部 dab 合成完毕

            std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 0);
            CHECK(dgcReadbackPixels(ctx, buf.data()) == DGC_OK, "readback OK");

            const auto px = [&](int x, int y) -> const std::uint8_t* {
                return &buf[((size_t)y * kW + (size_t)x) * 4];
            };
            const std::uint8_t* center = px(32, 32);  // 笔迹覆盖处 → 应变暗
            const std::uint8_t* corner = px(0, 0);    // 笔迹之外 → 仍白底
            std::fprintf(stderr,
                         "[test_brush_offscreen] center=(%d,%d,%d) corner=(%d,%d,%d)\n",
                         center[0], center[1], center[2], corner[0], corner[1], corner[2]);

            CHECK(center[0] < 200, "stroke darkened center (vs white bg)");
            CHECK(corner[0] > 200 && corner[1] > 200 && corner[2] > 200,
                  "corner stays white");

            dgcDestroy(ctx);
        }
    }

    // ── 2) 直接 VkBackend + 红色 StampData：逐 dab 颜色烘焙 ──
    {
        VkBackend backend;
        backend.initOffscreen(32, 32);
        backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);  // 白底

        std::vector<StampData> stamps;
        StampData s{};
        s.x = 16.0f;
        s.y = 16.0f;
        s.radius = 10.0f;
        s.hardness = 0.5f;
        s.opacity = 1.0f;
        s.r = 1.0f;  // 红
        s.g = 0.0f;
        s.b = 0.0f;
        stamps.push_back(s);
        backend.composite(stamps);

        std::vector<std::uint8_t> buf((size_t)32 * 32 * 4, 0);
        backend.readback(buf.data());
        const auto px = [&](int x, int y) -> const std::uint8_t* {
            return &buf[((size_t)y * 32 + (size_t)x) * 4];
        };
        const std::uint8_t* c = px(16, 16);
        std::fprintf(stderr, "[test_brush_offscreen] red stamp center=(%d,%d,%d)\n",
                     c[0], c[1], c[2]);
        CHECK(c[0] > 200 && c[1] < 50 && c[2] < 50, "red dab baked (r high, g/b low)");

        backend.shutdown();
    }

    if (failures == 0) {
        std::fprintf(stderr, "[test_brush_offscreen] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
