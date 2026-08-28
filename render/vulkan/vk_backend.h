#pragma once

#include <memory>
#include <mutex>

#ifdef DGCPAIN_TEST_HOOKS
#include <cstdint>
#endif

#include "core/interfaces/i_render_backend.h"

// VkBackend：真实 Vulkan 渲染后端（B2-1）。
//
// - 无窗口 headless 离屏模式（§4.0.5）：无 surface 扩展的 VkInstance + 设备 +
//   离屏 Canvas storage image（常驻 GENERAL），不创建 swapchain，present() no-op。
// - composite(stamps)：批量 dispatch brush_composite.comp 把 dab 形状光栅化 + over 合成到画布
//   （B4-1：compute 内 SDF + fwidth 覆盖，去掉 per-dab stamp 烘焙与纹理上传）。
// - readback：vkCmdCopyImageToBuffer + map 读回 RGBA8。
// - exportPNG：readback + stb_image_write 编码 PNG。
//
// 窗口/swapchain 路径本期不做（init 收到非空 surface 时记录未实现）。
// 内部用 std::mutex 串行化 GPU 提交与读回，避免 engine 渲染线程与 C API 线程竞态。
// Vulkan 句柄由 Impl 内最小 RAII 守卫（VkTopHandle/VkDeviceHandle）持有，按
// 「子对象 → device → instance」逆声明序析构；shutdown() 幂等（重复调用安全）。
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
    void clearCanvas(float r, float g, float b, float a) override;
    void present() override;
    void shutdown() override;

    // 离屏模式。
    bool supportsOffscreen() const noexcept override { return true; }
    void initOffscreen(int w, int h) override;
    void readback(void* rgbaOut) override;
    void exportPNG(const char* path) override;

#ifdef DGCPAIN_TEST_HOOKS
    // Bug3（dab 合成孔洞）回归（仅测试编译可见）：CompositeLocked 里 dispatch/barrier 的
    // 实际调用计数，供 test_composite_barrier_repro 断言「每次 dispatch 后都跟了一次 barrier」。
    // 生产构建（未定义 DGCPAIN_TEST_HOOKS）看不到这两个方法，也零开销。
    std::uint64_t testDispatchCount() const;
    std::uint64_t testBarrierCount() const;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // mutable：testDispatchCount/testBarrierCount 两个 const 测试 hook 需要加锁读计数。
    mutable std::mutex mutex_;  // 串行化 GPU 提交 / 读回（render 线程 vs C API 线程）。
};
