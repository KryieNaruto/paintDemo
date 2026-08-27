#pragma once

#include <memory>

#include "third_party/renderdoc/renderdoc_app.h"

// RenderDocCapture：RenderDoc 程序化抓帧（内部模块，不进 C ABI）。
//
// 运行时动态加载 RenderDoc 库、经 RENDERDOC_GetAPI 取 RENDERDOC_API_1_1_1，供
// VkBackend 在 composite dispatch 前后包 StartFrameCapture/EndFrameCapture，使纯
// 离屏（无 swapchain、无 vkQueuePresentKHR、独立 VkDevice）的内核管线无 present 也可被抓。
//
// - 默认关：构造读环境变量 DGC_RENDERDOC（非空且非 "0" 才启用）；未启用 / 库不存在 /
//   符号缺失 / 版本不符 → 全程 no-op，不影响现有离屏路径（零回归）。
// - 所有权 RAII：库句柄由 std::unique_ptr<void, LibCloser> 持有（Windows FreeLibrary /
//   POSIX dlclose），api_ 为 DLL 持有的借用指针（非拥有）。
class RenderDocCapture {
public:
    RenderDocCapture();
    ~RenderDocCapture();

    RenderDocCapture(const RenderDocCapture&) = delete;
    RenderDocCapture& operator=(const RenderDocCapture&) = delete;

    // 是否已启用且可用（env 开关 + 库加载 + GetAPI 成功）。
    bool available() const noexcept { return available_; }

    // device 为 RENDERDOC_DevicePointer（void*）。Vulkan 下由调用方按
    // RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE 转换后传入（VkInstance dispatch table 指针）。
    // device 为 nullptr 或不可用时 no-op。
    void startFrameCapture(void* device);
    void endFrameCapture(void* device);

private:
    struct LibCloser {
        void operator()(void* h) const noexcept;
    };

    void EnsureLoaded();
    void configureFromEnv();

    // 借用指针先于拥有句柄声明（析构逆序：先卸载 lib_、后平凡析构 api_），
    // 且不在析构中解引用 api_（避免 dangling 访问）。
    const RENDERDOC_API_1_1_1* api_ = nullptr;
    std::unique_ptr<void, LibCloser> lib_;
    bool enabled_ = false;    // env 开关 DGC_RENDERDOC 置位
    bool available_ = false;  // 库加载 + GetAPI 成功
    bool tried_ = false;      // 懒加载只尝试一次（失败后不再重试）
};
