#pragma once

#include <memory>

#include "core/interfaces/i_render_backend.h"

// 后端工厂：隔离 Vulkan/shaderc 依赖，让 sdk_api 等调用方只依赖本头，
// 不直接 include 任何 Vulkan 头。Vulkan 开时返回 VkBackend，否则返回 Null 桩。
// 所有权：返回 std::unique_ptr，由调用方接管后端生命周期（B1-8 所有权重构）。
std::unique_ptr<IRenderBackend> CreateDefaultRenderBackend();
