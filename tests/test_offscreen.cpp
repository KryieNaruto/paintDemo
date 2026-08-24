/* B2-1 host ctest：真实 Vulkan 离屏合成验证。
 *
 * 不经过 IPaintKernel / IPlatform / 窗口：直接构造 VkBackend，
 * initOffscreen → clearCanvas → composite(固定 stamp) → readback →
 * 断言像素相对底色变化 → exportPNG → shutdown。
 * host 用 lavapipe 软件光栅 headless 运行，无需显示服务器。
 */
#include <cstdint>
#include <cstdio>
#include <vector>

#include "render/vulkan/vk_backend.h"

static int failures = 0;

#define CHECK(cond, name)                     \
    do {                                      \
        if (!(cond)) {                        \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                       \
        }                                     \
    } while (0)

int main() {
    constexpr int kW = 64;
    constexpr int kH = 64;

    VkBackend backend;
    backend.initOffscreen(kW, kH);
    backend.clearCanvas();  // 清成不透明白底

    // 固定 stamp：中心 (32,32)、半径 16、硬度 0.5、不透明度 1.0。
    std::vector<StampData> stamps;
    stamps.push_back(StampData{32.0f, 32.0f, 16.0f, 0.5f, 1.0f});
    backend.composite(stamps);

    std::vector<uint8_t> buf((size_t)kW * kH * 4, 0);
    backend.readback(buf.data());

    const auto px = [&](int x, int y) -> const uint8_t* {
        return &buf[((size_t)y * kW + (size_t)x) * 4];
    };

    const uint8_t* center = px(32, 32);  // stamp 中心 → 应明显变暗
    const uint8_t* corner = px(0, 0);    // stamp 包围盒之外 → 应仍为白底
    const uint8_t* mid    = px(40, 32);  // stamp 内边缘区域 → 应相对白底变化

    std::fprintf(stderr, "[test_offscreen] center=(%d,%d,%d,%d) corner=(%d,%d,%d,%d) mid=(%d,%d,%d,%d)\n",
                 center[0], center[1], center[2], center[3],
                 corner[0], corner[1], corner[2], corner[3],
                 mid[0], mid[1], mid[2], mid[3]);

    // 中心像素相对白底（255）变暗。
    CHECK(center[0] < 200, "center pixel darkened (r < 200)");
    // 角落像素仍在 stamp 包围盒之外，保持白底。
    CHECK(corner[0] > 200 && corner[1] > 200 && corner[2] > 200, "corner stays white");
    // 中圈（软边内）也应相对白底变化（验证软圆 alpha 生效）。
    CHECK(mid[0] < 250, "mid pixel changed vs background");

    backend.exportPNG("/tmp/b2_1_offscreen.png");
    backend.shutdown();

    if (failures == 0) {
        std::fprintf(stderr, "[test_offscreen] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
