#include "core/null/null_render_backend.h"

void NullRenderBackend::init(PlatformSurface, int, int) {}

void NullRenderBackend::resize(int, int) {}

void NullRenderBackend::beginFrame() {}

void NullRenderBackend::composite(const std::vector<StampData>&, bool) {}

void NullRenderBackend::clearTip() {}

void NullRenderBackend::clearCanvas(float, float, float, float) {}

void NullRenderBackend::present() {}

void NullRenderBackend::shutdown() {}

void NullRenderBackend::initOffscreen(int, int) {}

void NullRenderBackend::readback(void*) {}

void NullRenderBackend::exportPNG(const char*) {}
