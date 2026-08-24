#pragma once

#include "core/interfaces/i_render_backend.h"

// NullRenderBackend：IRenderBackend 的空跑桩，所有方法为空操作。
class NullRenderBackend : public IRenderBackend {
public:
    void init(PlatformSurface surface, int w, int h) override;
    void resize(int w, int h) override;
    void beginFrame() override;
    void composite(const std::vector<StampData>& stamps) override;
    void clearCanvas() override;
    void present() override;
    void shutdown() override;
};
