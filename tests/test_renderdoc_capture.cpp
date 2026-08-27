/* B5-4 host ctest：RenderDoc 程序化抓帧 —— 降级路径 + 所有权循环。
 *
 * 无 librenderdoc.so 环境下 DGC_RENDERDOC=1 应走降级路径（加载失败 → no-op），
 * 一次 offscreen composite 的像素与「未设置 DGC_RENDERDOC」时逐字节一致（降级等价）。
 * 同时多轮 create/destroy（VkBackend 内含 RenderDocCapture 包装对象反复构造/析构），
 * 由 ASan/LSan 兜底验证库句柄 / 包装对象无泄漏。
 *
 * host 用 lavapipe 软件光栅 headless 运行，无需显示服务器，也无需安装 RenderDoc。
 */
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "render/vulkan/vk_backend.h"

static int failures = 0;

#define CHECK(cond, name)                              \
    do {                                               \
        if (!(cond)) {                                 \
            std::fprintf(stderr, "FAIL: %s\n", name);  \
            ++failures;                                \
        }                                              \
    } while (0)

static void setEnvVar(const char* name, const char* value) {
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

static void unsetEnvVar(const char* name) {
#ifdef _WIN32
    _putenv_s(name, "");
#else
    unsetenv(name);
#endif
}

// 跑一次 offscreen 会话（clear → composite 固定 stamp → readback），返回 RGBA8 像素。
static std::vector<uint8_t> runSession(bool enableCapture) {
    constexpr int kW = 64;
    constexpr int kH = 64;

    if (enableCapture) {
        setEnvVar("DGC_RENDERDOC", "1");
    } else {
        unsetEnvVar("DGC_RENDERDOC");
    }

    VkBackend backend;
    backend.initOffscreen(kW, kH);
    backend.clearCanvas();

    std::vector<StampData> stamps;
    stamps.push_back(StampData{32.0f, 32.0f, 16.0f, 0.5f, 1.0f});
    backend.composite(stamps);

    std::vector<uint8_t> buf((size_t)kW * kH * 4, 0);
    backend.readback(buf.data());
    backend.shutdown();
    return buf;
}

int main() {
    // 降级等价：DGC_RENDERDOC 未设置 vs 设置（无 renderdoc 库 → 均 no-op）。
    std::vector<uint8_t> baseline = runSession(false);
    std::vector<uint8_t> withEnv = runSession(true);

    CHECK(!baseline.empty() && baseline.size() == withEnv.size(), "readback size consistent");
    CHECK(baseline == withEnv, "pixels identical with/without DGC_RENDERDOC (degraded no-op)");

    // 所有权循环：多轮 create/destroy，包装对象（RenderDocCapture + VkBackend）反复构造/析构。
    for (int i = 0; i < 32; ++i) {
        std::vector<uint8_t> buf = runSession((i % 2) == 0);
        if (buf.empty()) {
            CHECK(false, "ownership loop readback non-empty");
        }
    }
    CHECK(true, "ownership loop completed");

    if (failures == 0) {
        std::fprintf(stderr, "[test_renderdoc_capture] PASS\n");
    }
    return failures == 0 ? 0 : 1;
}
