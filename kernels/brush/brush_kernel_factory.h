#pragma once

#include <memory>

class IPaintKernel;

// 内核工厂（B3-1，镜像 render/render_backend_factory）：
// DGCPAIN_KERNEL_BRUSH 开 → BrushKernel，否则 NullPaintKernel。
// 所有权：返回 std::unique_ptr，由调用方接管内核生命周期。
std::unique_ptr<IPaintKernel> CreateDefaultPaintKernel();
