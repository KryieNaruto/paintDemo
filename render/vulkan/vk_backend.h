#pragma once

#include <memory>
#include <mutex>

#include "core/interfaces/i_render_backend.h"

// VkBackend：真实 Vulkan 渲染后端（B2-1）。
//
// - 无窗口 headless 离屏模式（§4.0.5）：无 surface 扩展的 VkInstance + 设备 +
//   离屏 Canvas storage image（常驻 GENERAL），不创建 swapchain，present() no-op。
// - composite(stamps)：批量 dispatch brush_composite.comp 把 stamp 合成到画布。
// - readback：vkCmdCopyImageToBuffer + map 读回 RGBA8。
// - exportPNG：readback + stb_image_write 编码 PNG。
//
// 窗口/swapchain 路径本期不做（init 收到非空 surface 时记录未实现）。
// 内部用 std::mutex 串行化 GPU 提交与读回，避免 engine 渲染线程与 C API 线程竞态。
class VkBackend : public IRenderBackend {
public:
    VkBackend();
    ~VkBackend() override;

    VkBackend(const VkBackend&) = delete;
    VkBackend& operator=(const VkBackend&) = delete;

    // 窗口模式（保留接口，本期仅支持 surface==nullptr → 委托离屏）。
    void init(PlatformSurface surface, int w, int h) override;
    void resize(int w, int h) override;
    void beginFrame() override;
    void composite(const std::vector<StampData>& stamps) override;
    void clearCanvas() override;
    void present() override;
    void shutdown() override;

    // 离屏模式。
    bool supportsOffscreen() const noexcept override { return true; }
    void initOffscreen(int w, int h) override;
    void readback(void* rgbaOut) override;
    void exportPNG(const char* path) override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::mutex mutex_;  // 串行化 GPU 提交 / 读回（render 线程 vs C API 线程）。
};
