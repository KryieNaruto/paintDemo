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
    virtual void clearCanvas() = 0;
    virtual void present() = 0;
    virtual void shutdown() = 0;
};
