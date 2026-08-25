#include "render/render_backend_factory.h"

#include <memory>

#include "core/null/null_render_backend.h"

#ifdef DGCPAIN_HAVE_VULKAN
#include "render/vulkan/vk_backend.h"
#endif

std::unique_ptr<IRenderBackend> CreateDefaultRenderBackend() {
#ifdef DGCPAIN_HAVE_VULKAN
    return std::make_unique<VkBackend>();
#else
    return std::make_unique<NullRenderBackend>();
#endif
}
