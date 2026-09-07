#pragma once

#include "core/interfaces/i_render_backend.h"

// NullRenderBackend：IRenderBackend 的空跑桩，所有方法为空操作。
class NullRenderBackend : public IRenderBackend {
public:
    void init(PlatformSurface surface, int w, int h) override;
    void resize(int w, int h) override;
    void beginFrame() override;
    void composite(const std::vector<StampData>& stamps, bool predicted = false) override;
    void clearTip() override;
    void clearCanvas(float r, float g, float b, float a) override;
    void present() override;
    void shutdown() override;

    // 离屏三函数空桩（supportsOffscreen 用基类默认 false）。
    void initOffscreen(int w, int h) override;
    void readback(void* rgbaOut) override;
    void exportPNG(const char* path) override;
};
