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

    // §4.0.5 离屏模式：无窗口渲染到 storage image + 读回/导出 PNG。
    // 默认不支持（Null 桩等）；VkBackend 覆写为 true。
    virtual bool supportsOffscreen() const noexcept { return false; }
    virtual void initOffscreen(int w, int h) = 0;
    virtual void readback(void* rgbaOut) = 0;
    virtual void exportPNG(const char* path) = 0;
};
