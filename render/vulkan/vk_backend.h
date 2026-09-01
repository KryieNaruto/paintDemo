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

    // Bug #3（快照刷新节流）：消费端想要新鲜 readback 快照时置位。非阻塞——只 store 一个
    // 原子标志，不碰 GPU、不等渲染线程；标志在下次 composite 批提交末尾由 CompositeLocked
    // 消费（exchange(false)），若已置位才实际执行全画布 GPU→CPU 快照拷贝。这样连续绘制时
    // overCap 自动合批的 composite 不再无条件付 3.1MB 拷贝（Mali 弱 GPU 饱和 → 60→30 掉帧
    // 的根因），快照刷新频率从「每 composite 一次」降为「每请求一次」。
    void requestSnapshotRefresh() override;
    // Bug #3（快照刷新节流）drain 收尾：同步强制刷新一次快照缓存（加锁执行
    // RefreshReadbackCacheLocked），把「最后一次 composite 之后的输入尾部」捕获进快照。
    // 仅由阻塞的 dgcFlush 调用（非每帧路径）。
    void flushReadbackCache() override;

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

    // Bug #3（快照刷新节流）回归（仅测试编译可见）：快照刷新计数（每次实际全画布 GPU→CPU
    // 快照拷贝 +1）与 composite 批提交计数（每次非空 CompositeLocked +1），供
    // test_snapshot_refresh_throttle 断言「刷新频率从每 composite 一次降为每请求/结算一次」。
    std::uint64_t testSnapshotRefreshCount() const;
    std::uint64_t testCompositeCount() const;
#endif

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    // mutable：testDispatchCount/testBarrierCount 两个 const 测试 hook 需要加锁读计数。
    mutable std::mutex mutex_;  // 串行化 GPU 提交 / 读回（render 线程 vs C API 线程）。
};
