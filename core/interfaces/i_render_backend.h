#pragma once

#include <vector>

#include "core/types.h"

// IRenderBackend：渲染后端抽象（StampData → 画布合成 → 上屏），纯虚接口，不对外。
class IRenderBackend {
public:
    virtual ~IRenderBackend() = default;
    virtual void init(PlatformSurface, int w, int h) = 0;
    virtual void resize(int w, int h) = 0;
    virtual void beginFrame() = 0;
    virtual void composite(const std::vector<StampData>&) = 0;
    virtual void clearCanvas(float r, float g, float b, float a) = 0;
    virtual void present() = 0;
    virtual void shutdown() = 0;

    // Bug #3（快照刷新节流）：消费端请求一次新鲜的 readback 快照。默认空实现（Null 桩等
    // 不维护快照缓存）；VkBackend 覆写为「置位一个非阻塞原子标志」，由下一次 composite 批
    // 提交末尾消费——连续绘制中 overCap 自动合批的 composite 不再无条件付全画布 GPU→CPU
    // 拷贝，快照刷新频率从「每 composite 一次」降为「每请求一次」。
    virtual void requestSnapshotRefresh() {}

    // Bug #3（快照刷新节流）drain 收尾：**同步**强制刷新一次 readback 快照缓存（全画布
    // GPU→CPU 拷贝），把「最后一次 composite 之后的输入尾部」捕获进快照。默认空实现
    // （Null 桩等）；VkBackend 覆写为加锁执行 RefreshReadbackCacheLocked。仅由阻塞的
    // dgcFlush（Engine::flush）在排空完成后调用——该路径本就阻塞、非每帧，追加一次 GPU
    // 等待与既有语义一致（非每帧路径，无 20fps 回退风险）。
    virtual void flushReadbackCache() {}

    // §4.0.5 离屏模式：无窗口渲染到 storage image + 读回/导出 PNG。
    // 默认不支持（Null 桩等）；VkBackend 覆写为 true。
    virtual bool supportsOffscreen() const noexcept { return false; }
    virtual void initOffscreen(int w, int h) = 0;
    virtual void readback(void* rgbaOut) = 0;
    virtual void exportPNG(const char* path) = 0;
};
