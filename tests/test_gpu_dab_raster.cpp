// B4-1 host ctest（Vulkan 开）：dab 形状光栅化 GPU 化对照。
//
// 同一组 StampData：GPU 光栅化（compute 内 SDF + fwidth/解析 AA 覆盖 + over 合成）
// vs CPU 参考（tests/cpu_stamp_reference.h，独立 oracle）逐像素 diff ≤ 8/255。
// 双证笔迹可见（中心变暗/着色 + 包围盒外白底）+ shapeType 默认圆形对称 + 多轮泄漏循环。
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/types.h"
#include "render/vulkan/vk_backend.h"
#include "tests/cpu_stamp_reference.h"

static int failures = 0;
#define CHECK(cond, name)                       \
    do {                                        \
        if (!(cond)) {                          \
            std::fprintf(stderr, "FAIL: %s\n", name); \
            ++failures;                         \
        }                                       \
    } while (0)

namespace {

constexpr int kW = 64;
constexpr int kH = 64;

std::vector<std::uint8_t> MakeWhite() {
    std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 255);
    return buf;
}

const std::uint8_t* px(const std::vector<std::uint8_t>& buf, int x, int y) {
    return &buf[((size_t)y * kW + (size_t)x) * 4];
}

// 对单个 dab 做一次 GPU vs CPU 对照，返回最大逐通道像素差（0..255）。
int DiffOneDab(const StampData& s) {
    VkBackend backend;
    backend.initOffscreen(kW, kH);
    backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);  // 不透明白底
    backend.composite({s});
    std::vector<std::uint8_t> gpu((size_t)kW * kH * 4, 0);
    backend.readback(gpu.data());
    backend.shutdown();

    std::vector<std::uint8_t> cpu = MakeWhite();
    cpu_ref::CompositeOverCpu(cpu.data(), kW, kH, s);

    int maxDiff = 0;
    for (size_t i = 0; i < gpu.size(); ++i) {
        const int d = std::abs((int)gpu[i] - (int)cpu[i]);
        if (d > maxDiff) {
            maxDiff = d;
        }
    }
    return maxDiff;
}

}  // namespace

int main() {
    // 覆盖不同 radius / hardness / softness / opacity / rgb 的一组 StampData。
    struct Case {
        const char* name;
        StampData s;
    };
    const Case cases[] = {
        {"big black hard", StampData{32.0f, 32.0f, 16.0f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
        {"small red", StampData{32.0f, 32.0f, 4.0f, 0.8f, 0.7f, 1.0f, 0.0f, 0.0f, 0.0f}},
        {"soft blue", StampData{32.0f, 32.0f, 12.0f, 0.5f, 1.0f, 0.0f, 0.0f, 1.0f, 0.4f}},
        {"soft green", StampData{32.0f, 32.0f, 8.0f, 1.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.2f}},
        {"hard black (rim AA)", StampData{32.0f, 32.0f, 10.0f, 1.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f}},
    };

    for (const Case& c : cases) {
        const int maxDiff = DiffOneDab(c.s);
        std::fprintf(stderr, "[test_gpu_dab_raster] %-22s maxDiff=%d\n", c.name, maxDiff);
        CHECK(maxDiff <= 8, c.name);
    }

    // 笔迹可见双证：用「big black hard」单独跑一次，断言中心变暗 + 包围盒外白底。
    {
        const StampData s{32.0f, 32.0f, 16.0f, 0.5f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        VkBackend backend;
        backend.initOffscreen(kW, kH);
        backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);
        backend.composite({s});
        std::vector<std::uint8_t> gpu((size_t)kW * kH * 4, 0);
        backend.readback(gpu.data());
        backend.shutdown();

        const std::uint8_t* center = px(gpu, 32, 32);
        const std::uint8_t* corner = px(gpu, 0, 0);
        std::fprintf(stderr,
                     "[test_gpu_dab_raster] center=(%d,%d,%d) corner=(%d,%d,%d)\n",
                     center[0], center[1], center[2], corner[0], corner[1], corner[2]);
        CHECK(center[0] < 200, "center darkened (black dab)");
        CHECK(corner[0] == 255 && corner[1] == 255 && corner[2] == 255, "corner stays white");
    }

    // shapeType=0 默认圆形：软边 dab（圆心落在像素中心，避免半像素不对称）
    // 在 +x/-x/+y/-y 等距四点的覆盖对称（颜色一致）。
    {
        const StampData s{32.5f, 32.5f, 12.0f, 0.3f, 1.0f, 0.0f, 0.0f, 0.0f, 0.2f};
        VkBackend backend;
        backend.initOffscreen(kW, kH);
        backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);
        backend.composite({s});
        std::vector<std::uint8_t> gpu((size_t)kW * kH * 4, 0);
        backend.readback(gpu.data());
        backend.shutdown();

        const auto eq4 = [&](int x0, int y0, int x1, int y1, int x2, int y2, int x3, int y3) {
            const std::uint8_t* a = px(gpu, x0, y0);
            const std::uint8_t* b = px(gpu, x1, y1);
            const std::uint8_t* d = px(gpu, x2, y2);
            const std::uint8_t* e = px(gpu, x3, y3);
            return a[0] == b[0] && b[0] == d[0] && d[0] == e[0] &&
                   a[1] == b[1] && b[1] == d[1] && d[1] == e[1] &&
                   a[2] == b[2] && b[2] == d[2] && d[2] == e[2];
        };
        CHECK(eq4(40, 32, 24, 32, 32, 40, 32, 24), "circle symmetric (+x/-x/+y/-y)");
    }

    // 多轮 init/composite/readback/shutdown 泄漏循环（供 ASan/LSan）。
    for (int round = 0; round < 8; ++round) {
        VkBackend backend;
        backend.initOffscreen(kW, kH);
        backend.clearCanvas(1.0f, 1.0f, 1.0f, 1.0f);
        StampData s{32.0f, 32.0f, 10.0f, 0.5f, 1.0f, 0.2f, 0.4f, 0.6f, 0.1f};
        backend.composite({s});
        std::vector<std::uint8_t> buf((size_t)kW * kH * 4, 0);
        backend.readback(buf.data());
        backend.shutdown();
    }
    std::fprintf(stderr, "[test_gpu_dab_raster] leak loop 8x done\n");

    if (failures == 0) {
        std::fprintf(stderr, "[test_gpu_dab_raster] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
