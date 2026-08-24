#include "render/render_backend_factory.h"

#include "core/null/null_render_backend.h"

#ifdef DGCPAIN_HAVE_VULKAN
#include "render/vulkan/vk_backend.h"
#endif

IRenderBackend* CreateDefaultRenderBackend() {
#ifdef DGCPAIN_HAVE_VULKAN
    return new VkBackend();
#else
    return new NullRenderBackend();
#endif
}
