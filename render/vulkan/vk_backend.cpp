#include "render/vulkan/vk_backend.h"

#include <vulkan/vulkan.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef DGCPAIN_PRECOMPILED_SPV
#include "brush_composite_spv.h"  // Android：构建期 glslc 预编译内嵌（单一权威源仍是 .comp）
#else
#include <shaderc/shaderc.hpp>
#include "brush_composite_glsl.h"  // 由 CMake 从 brush_composite.comp 生成（单一权威源）
#endif

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "third_party/stb/stb_image_write.h"

namespace {

constexpr VkFormat kCanvasFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkFormat kStampFormat  = VK_FORMAT_R8G8B8A8_UNORM;

// 与 brush_composite.comp 的 push_constant 布局一一对应。
struct BrushPushConstant {
    float stampPos[2];
    float stampSize[2];
    float opacity;
    float alphaLock;
};
static_assert(sizeof(BrushPushConstant) == 6 * sizeof(float), "push constant size");

// §4.5 GLSL → SPIR-V：host 用 shaderc 库在代码内编译（不 shell 调 glslc/glslangValidator）；
// Android（NDK 无 libshaderc）改走构建期 glslc 预编译的内嵌 SPIR-V（DGCPAIN_PRECOMPILED_SPV）。
std::vector<uint32_t> CompileBrushShader(std::string* err) {
#ifdef DGCPAIN_PRECOMPILED_SPV
    (void)err;  // 无运行时编译错误路径
    const size_t nbytes = sizeof(kBrushCompositeSpv);
    // SPIR-V 指令长度按 4 字节对齐（glslc 产出天然 4 对齐）。
    static_assert(sizeof(kBrushCompositeSpv) % sizeof(uint32_t) == 0,
                  "embedded SPIR-V byte array must be 4-byte aligned");
    std::vector<uint32_t> spv(nbytes / sizeof(uint32_t));
    std::memcpy(spv.data(), kBrushCompositeSpv, nbytes);
    return spv;
#else
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        kBrushCompositeGlsl, std::strlen(kBrushCompositeGlsl),
        shaderc_compute_shader, "brush_composite.comp", options);
    if (result.GetCompilationStatus() != shaderc_compilation_status_success) {
        if (err) {
            *err = result.GetErrorMessage();
        }
        return {};
    }
    return std::vector<uint32_t>(result.begin(), result.end());
#endif
}

uint32_t FindMemoryType(VkPhysicalDevice phys, uint32_t typeFilter, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        if ((typeFilter & (1u << i)) &&
            (memProps.memoryTypes[i].propertyFlags & props) == props) {
            return i;
        }
    }
    return 0;
}

void CreateImage(VkDevice device, VkPhysicalDevice phys, uint32_t w, uint32_t h, VkFormat format,
                 VkImageUsageFlags usage, VkImageLayout initialLayout,
                 VkImage& image, VkDeviceMemory& memory) {
    VkImageCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.initialLayout = initialLayout;
    if (vkCreateImage(device, &info, nullptr, &image) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkCreateImage failed\n");
        return;
    }
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, image, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(phys, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkAllocateMemory(image) failed\n");
        return;
    }
    vkBindImageMemory(device, image, memory, 0);
}

VkImageView CreateImageView(VkDevice device, VkImage image, VkFormat format) {
    VkImageViewCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    info.image = image;
    info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    info.format = format;
    info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    info.subresourceRange.baseMipLevel = 0;
    info.subresourceRange.levelCount = 1;
    info.subresourceRange.baseArrayLayer = 0;
    info.subresourceRange.layerCount = 1;
    VkImageView view = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &info, nullptr, &view) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkCreateImageView failed\n");
    }
    return view;
}

void CreateBuffer(VkDevice device, VkPhysicalDevice phys, VkDeviceSize size, VkBufferUsageFlags usage,
                  VkMemoryPropertyFlags props, VkBuffer& buffer, VkDeviceMemory& memory) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkCreateBuffer failed\n");
        return;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = FindMemoryType(phys, req.memoryTypeBits, props);
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkAllocateMemory(buffer) failed\n");
        return;
    }
    vkBindBufferMemory(device, buffer, memory, 0);
}

void TransitionImageLayout(VkCommandBuffer cmd, VkImage image,
                           VkImageLayout oldLayout, VkImageLayout newLayout) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    VkPipelineStageFlags dstStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        srcStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        dstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        srcStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        dstStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    }
    vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);
}

// 程序化软圆 alpha 位图（B3-1 起按逐 dab 颜色烘焙，不再固定黑）。
// RGB 取 s.r/s.g/s.b（straight RGB，0..1），A 为软圆 alpha（hardness 控硬核、softness 控外缘软化）。
// softness=0 时与 B2-1 行为完全一致（hardness 内实心 → 外缘线性斜坡）。
std::vector<uint8_t> MakeSoftCircleStamp(const StampData& s, uint32_t* outSize) {
    int d = (int)std::ceil(s.radius * 2.0f);
    if (d < 1) {
        d = 1;
    }
    if (d > 512) {
        d = 512;  // 占位 stamp 上限，防内存失控
    }
    *outSize = (uint32_t)d;
    std::vector<uint8_t> px((size_t)d * d * 4, 0);

    const float r = s.radius > 0.0f ? s.radius : 1.0f;
    float hardness = s.hardness;
    if (hardness < 0.0f) {
        hardness = 0.0f;
    }
    if (hardness > 0.999f) {
        hardness = 0.999f;
    }
    float softness = s.softness;
    if (softness < 0.0f) {
        softness = 0.0f;
    }
    if (softness > 0.999f) {
        softness = 0.999f;
    }
    // 有效硬核边界：softness 越大硬核越小、外缘越软；softness=0 → edge=hardness（B2-1 兼容）。
    const float edge = hardness * (1.0f - softness);

    // straight RGB → 8bit（clamp 到 [0,1]）。
    const auto to8 = [](float c) -> uint8_t {
        c = c < 0.0f ? 0.0f : (c > 1.0f ? 1.0f : c);
        return (uint8_t)(c * 255.0f + 0.5f);
    };
    const uint8_t cr = to8(s.r);
    const uint8_t cg = to8(s.g);
    const uint8_t cb = to8(s.b);

    const float center = (float)(d - 1) * 0.5f;
    for (int y = 0; y < d; ++y) {
        for (int x = 0; x < d; ++x) {
            const float dx = ((float)x + 0.5f - center) / r;
            const float dy = ((float)y + 0.5f - center) / r;
            const float dist = std::sqrt(dx * dx + dy * dy);
            float alpha = 0.0f;
            if (dist <= edge) {
                alpha = 1.0f;
            } else if (dist < 1.0f) {
                const float t = (1.0f - dist) / (1.0f - edge);
                alpha = t * t * (3.0f - 2.0f * t);  // smoothstep
            }
            const uint8_t a = (uint8_t)(alpha * 255.0f + 0.5f);
            const size_t i = ((size_t)y * (size_t)d + (size_t)x) * 4;
            px[i + 0] = cr;  // r（逐 dab 颜色）
            px[i + 1] = cg;  // g
            px[i + 2] = cb;  // b
            px[i + 3] = a;   // a
        }
    }
    return px;
}

// ── 最小手写 RAII 守卫（B1-8，不引入 vk::raii）：值语义、禁拷贝、可移动、
//    reset()/析构幂等。 ──

// 顶层对象：vkDestroyInstance / vkDestroyDevice（2 参，无 parent device）。
template <typename H, void (*Destroy)(H, const VkAllocationCallbacks*)>
struct VkTopHandle {
    H h = VK_NULL_HANDLE;

    VkTopHandle() = default;
    VkTopHandle(const VkTopHandle&) = delete;
    VkTopHandle& operator=(const VkTopHandle&) = delete;
    VkTopHandle(VkTopHandle&& other) noexcept : h(other.h) { other.h = VK_NULL_HANDLE; }
    VkTopHandle& operator=(VkTopHandle&& other) noexcept {
        if (this != &other) {
            reset();
            h = other.h;
            other.h = VK_NULL_HANDLE;
        }
        return *this;
    }
    ~VkTopHandle() { reset(); }

    H get() const { return h; }
    operator H() const { return h; }

    void reset() {
        if (h != VK_NULL_HANDLE) {
            Destroy(h, nullptr);
            h = VK_NULL_HANDLE;
        }
    }
    H release() {
        H t = h;
        h = VK_NULL_HANDLE;
        return t;
    }
    // 收编新裸句柄：先销毁旧值，再记录新值。
    VkTopHandle& operator=(H handle) {
        reset();
        h = handle;
        return *this;
    }
};

// 子对象：vkDestroyXxx / vkFreeMemory（3 参，需 device）。
// commandBuffer / descriptorSet 属 commandPool / descriptorPool 子句柄，随池守卫析构
// 隐式释放，无需独立守卫（仍以裸句柄存于 Impl）。
template <typename H, void (*Destroy)(VkDevice, H, const VkAllocationCallbacks*)>
struct VkDeviceHandle {
    VkDevice dev = VK_NULL_HANDLE;
    H h = VK_NULL_HANDLE;

    VkDeviceHandle() = default;
    VkDeviceHandle(const VkDeviceHandle&) = delete;
    VkDeviceHandle& operator=(const VkDeviceHandle&) = delete;
    VkDeviceHandle(VkDeviceHandle&& other) noexcept : dev(other.dev), h(other.h) {
        other.h = VK_NULL_HANDLE;
    }
    VkDeviceHandle& operator=(VkDeviceHandle&& other) noexcept {
        if (this != &other) {
            reset();
            dev = other.dev;
            h = other.h;
            other.h = VK_NULL_HANDLE;
        }
        return *this;
    }
    ~VkDeviceHandle() { reset(); }

    H get() const { return h; }
    operator H() const { return h; }

    void reset() {
        if (h != VK_NULL_HANDLE) {
            Destroy(dev, h, nullptr);
            h = VK_NULL_HANDLE;
        }
    }
    H release() {
        H t = h;
        h = VK_NULL_HANDLE;
        return t;
    }
    // 收编新句柄：先销毁旧句柄，再记录设备与新句柄。
    void assign(VkDevice d, H handle) {
        reset();
        dev = d;
        h = handle;
    }
};

}  // namespace

struct VkBackend::Impl {
    // 声明序 = instance → device → 全部子对象：逆声明序析构 = 子对象 → device → instance，
    // 即 Vulkan 释放顺序正确。commandBuffer/descriptorSet 属池子句柄，不独立守卫。
    VkTopHandle<VkInstance, vkDestroyInstance> instance;
    VkTopHandle<VkDevice, vkDestroyDevice> device;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkQueue queue = VK_NULL_HANDLE;

    VkDeviceHandle<VkCommandPool, vkDestroyCommandPool> commandPool;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;

    VkDeviceHandle<VkFence, vkDestroyFence> fence;
    VkDeviceHandle<VkSampler, vkDestroySampler> sampler;
    VkDeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> descriptorLayout;
    VkDeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout> pipelineLayout;
    VkDeviceHandle<VkPipeline, vkDestroyPipeline> pipeline;

    VkDeviceHandle<VkImage, vkDestroyImage> canvasImage;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> canvasMemory;
    VkDeviceHandle<VkImageView, vkDestroyImageView> canvasView;
    int width = 0;
    int height = 0;

    VkDeviceHandle<VkImage, vkDestroyImage> stampImage;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> stampMemory;
    VkDeviceHandle<VkImageView, vkDestroyImageView> stampView;
    uint32_t stampSize = 0;
    VkImageLayout stampLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkDeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool> descriptorPool;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkDeviceHandle<VkBuffer, vkDestroyBuffer> stagingBuffer;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> stagingMemory;
    VkDeviceSize stagingSize = 0;

    VkDeviceHandle<VkBuffer, vkDestroyBuffer> readbackBuffer;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> readbackMemory;
    VkDeviceSize readbackSize = 0;

    bool deviceReady = false;
    bool canvasReady = false;

    void EnsureDevice() {
        if (device != VK_NULL_HANDLE) {
            return;
        }

        VkApplicationInfo appInfo{};
        appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        appInfo.pApplicationName = "dgc_paint";
        appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.pEngineName = "dgc_paint";
        appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
        appInfo.apiVersion = VK_API_VERSION_1_1;

        // 离屏：无 surface 扩展、无 layer。
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&instanceInfo, nullptr, &inst) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateInstance failed\n");
            return;
        }
        instance = inst;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(instance, &count, nullptr);
        if (count == 0) {
            std::fprintf(stderr, "[VkBackend] no physical device (lavapipe?) found\n");
            return;
        }
        std::vector<VkPhysicalDevice> devs(count);
        vkEnumeratePhysicalDevices(instance, &count, devs.data());

        // 选含 COMPUTE | TRANSFER | GRAPHICS 的 queue family（lavapipe 单队列即可）。
        physicalDevice = devs[0];
        bool foundFamily = false;
        for (VkPhysicalDevice dev : devs) {
            uint32_t famCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &famCount, nullptr);
            std::vector<VkQueueFamilyProperties> fams(famCount);
            vkGetPhysicalDeviceQueueFamilyProperties(dev, &famCount, fams.data());
            for (uint32_t i = 0; i < famCount; ++i) {
                const VkQueueFlags want = VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT;
                if ((fams[i].queueFlags & want) == want) {
                    physicalDevice = dev;
                    queueFamily = i;
                    foundFamily = true;
                    break;
                }
            }
            if (foundFamily) {
                break;
            }
        }
        if (!foundFamily) {
            std::fprintf(stderr, "[VkBackend] no compute|transfer|graphics queue family\n");
            return;
        }

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        // storage image 写入是 core，无需额外 feature（shader 用 rgba8 显式格式）。
        VkDevice dev = VK_NULL_HANDLE;
        if (vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &dev) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateDevice failed\n");
            return;
        }
        device = dev;
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = queueFamily;
        VkCommandPool cmdPool = VK_NULL_HANDLE;
        if (vkCreateCommandPool(device, &poolInfo, nullptr, &cmdPool) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateCommandPool failed\n");
            return;
        }
        commandPool.assign(device, cmdPool);

        VkCommandBufferAllocateInfo cbInfo{};
        cbInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbInfo.commandPool = commandPool;
        cbInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbInfo.commandBufferCount = 1;
        vkAllocateCommandBuffers(device, &cbInfo, &commandBuffer);

        VkFenceCreateInfo fenceInfo{};
        fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VkFence fenceH = VK_NULL_HANDLE;
        vkCreateFence(device, &fenceInfo, nullptr, &fenceH);
        fence.assign(device, fenceH);

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        samplerInfo.maxLod = 0.0f;
        VkSampler samplerH = VK_NULL_HANDLE;
        vkCreateSampler(device, &samplerInfo, nullptr, &samplerH);
        sampler.assign(device, samplerH);

        std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = (uint32_t)bindings.size();
        layoutInfo.pBindings = bindings.data();
        VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
        vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &dsl);
        descriptorLayout.assign(device, dsl);

        VkPushConstantRange pcRange{};
        pcRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcRange.offset = 0;
        pcRange.size = sizeof(BrushPushConstant);
        VkPipelineLayoutCreateInfo plInfo{};
        plInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plInfo.setLayoutCount = 1;
        VkDescriptorSetLayout setLayouts[] = {descriptorLayout};
        plInfo.pSetLayouts = setLayouts;
        plInfo.pushConstantRangeCount = 1;
        plInfo.pPushConstantRanges = &pcRange;
        VkPipelineLayout pl = VK_NULL_HANDLE;
        vkCreatePipelineLayout(device, &plInfo, nullptr, &pl);
        pipelineLayout.assign(device, pl);

        std::string err;
        std::vector<uint32_t> spv = CompileBrushShader(&err);
        if (spv.empty()) {
            std::fprintf(stderr, "[VkBackend] shaderc compile failed: %s\n", err.c_str());
            return;
        }

        VkShaderModuleCreateInfo moduleInfo{};
        moduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        moduleInfo.codeSize = spv.size() * sizeof(uint32_t);
        moduleInfo.pCode = spv.data();
        VkShaderModule module = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &moduleInfo, nullptr, &module) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateShaderModule failed\n");
            return;
        }

        VkComputePipelineCreateInfo pipeInfo{};
        pipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        pipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        pipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        pipeInfo.stage.module = module;
        pipeInfo.stage.pName = "main";
        pipeInfo.layout = pipelineLayout;
        VkPipeline pipe = VK_NULL_HANDLE;
        if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &pipe) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateComputePipelines failed\n");
        }
        pipeline.assign(device, pipe);
        vkDestroyShaderModule(device, module, nullptr);

        deviceReady = true;
    }

    void DestroyStampTexture() {
        // RAII 守卫 reset()：幂等，重复调用安全；析构顺序由声明序保证（子对象 → device）。
        stampView.reset();
        stampImage.reset();
        stampMemory.reset();
        stampSize = 0;
        stampLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    void DestroyCanvas() {
        // descriptorPool 销毁 → 隐式释放 descriptorSet（池子句柄，无独立守卫）。
        descriptorPool.reset();
        descriptorSet = VK_NULL_HANDLE;
        DestroyStampTexture();
        canvasView.reset();
        canvasImage.reset();
        canvasMemory.reset();
        readbackBuffer.reset();
        readbackMemory.reset();
        width = height = 0;
        readbackSize = 0;
        canvasReady = false;
    }

    void CreateCanvas(int w, int h) {
        DestroyCanvas();
        if (!deviceReady) {
            return;
        }
        if (w <= 0 || h <= 0) {
            return;
        }
        width = w;
        height = h;
        // Canvas storage image：usage 含 STORAGE|SAMPLED|TRANSFER_DST|TRANSFER_SRC，
        // 布局常驻 GENERAL（§4.0.5/§4.4）。
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        CreateImage(device, physicalDevice, (uint32_t)w, (uint32_t)h, kCanvasFormat,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, img, mem);
        canvasImage.assign(device, img);
        canvasMemory.assign(device, mem);
        canvasView.assign(device, CreateImageView(device, img, kCanvasFormat));
        if (img == VK_NULL_HANDLE || canvasView == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[VkBackend] canvas creation failed\n");
            return;
        }

        readbackSize = (VkDeviceSize)w * (VkDeviceSize)h * 4;
        VkBuffer rbuf = VK_NULL_HANDLE;
        VkDeviceMemory rmem = VK_NULL_HANDLE;
        CreateBuffer(device, physicalDevice, readbackSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     rbuf, rmem);
        readbackBuffer.assign(device, rbuf);
        readbackMemory.assign(device, rmem);
        if (rbuf == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[VkBackend] readback buffer creation failed\n");
            return;
        }

        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 1;
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[1].descriptorCount = 1;
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 1;
        poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
        poolInfo.pPoolSizes = poolSizes.data();
        VkDescriptorPool dpool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &dpool);
        descriptorPool.assign(device, dpool);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 1;
        VkDescriptorSetLayout setLayouts[] = {descriptorLayout};
        allocInfo.pSetLayouts = setLayouts;
        vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet);

        VkDescriptorImageInfo canvasInfo{};
        canvasInfo.imageView = canvasView;
        canvasInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        write.pImageInfo = &canvasInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);

        canvasReady = true;
    }

    void CreateStampTexture(uint32_t size) {
        if (stampSize == size && stampImage != VK_NULL_HANDLE) {
            return;
        }
        DestroyStampTexture();
        stampSize = size;
        VkImage img = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        CreateImage(device, physicalDevice, size, size, kStampFormat,
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    VK_IMAGE_LAYOUT_UNDEFINED, img, mem);
        stampImage.assign(device, img);
        stampMemory.assign(device, mem);
        stampView.assign(device, CreateImageView(device, img, kStampFormat));

        VkDeviceSize need = (VkDeviceSize)size * size * 4;
        if (need > stagingSize) {
            stagingBuffer.reset();
            stagingMemory.reset();
            VkBuffer buf = VK_NULL_HANDLE;
            VkDeviceMemory bmem = VK_NULL_HANDLE;
            CreateBuffer(device, physicalDevice, need, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         buf, bmem);
            stagingBuffer.assign(device, buf);
            stagingMemory.assign(device, bmem);
            stagingSize = need;
        }

        // 写 stamp binding（binding 1：combined image sampler）。
        VkDescriptorImageInfo stampInfo{};
        stampInfo.sampler = sampler;
        stampInfo.imageView = stampView;
        stampInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = descriptorSet;
        write.dstBinding = 1;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        write.pImageInfo = &stampInfo;
        vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
    }

    void SubmitAndWait() {
        if (vkEndCommandBuffer(commandBuffer) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkEndCommandBuffer failed\n");
            return;
        }
        vkResetFences(device, 1, &fence.h);
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &commandBuffer;
        if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkQueueSubmit failed\n");
            return;
        }
        vkWaitForFences(device, 1, &fence.h, VK_TRUE, UINT64_MAX);
    }

    void BeginCommands() {
        vkResetCommandBuffer(commandBuffer, 0);
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(commandBuffer, &begin);
    }

    void CompositeLocked(const std::vector<StampData>& stamps) {
        if (!canvasReady) {
            return;
        }
        if (stamps.empty()) {
            return;
        }
        BeginCommands();
        for (const StampData& s : stamps) {
            uint32_t size = 0;
            std::vector<uint8_t> px = MakeSoftCircleStamp(s, &size);
            CreateStampTexture(size);

            // 上传 staging → stamp 纹理。
            void* data = nullptr;
            vkMapMemory(device, stagingMemory, 0, stagingSize, 0, &data);
            std::memcpy(data, px.data(), px.size());
            vkUnmapMemory(device, stagingMemory);

            const VkImageLayout prevLayout = stampLayout;
            if (prevLayout != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
                TransitionImageLayout(commandBuffer, stampImage, prevLayout,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            }
            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = 0;
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {size, size, 1};
            vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, stampImage,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            TransitionImageLayout(commandBuffer, stampImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            stampLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                                    1, &descriptorSet, 0, nullptr);

            BrushPushConstant pc{};
            pc.stampPos[0] = s.x - s.radius;
            pc.stampPos[1] = s.y - s.radius;
            pc.stampSize[0] = s.radius * 2.0f;
            pc.stampSize[1] = s.radius * 2.0f;
            pc.opacity = s.opacity;
            pc.alphaLock = 0.0f;
            vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(pc), &pc);

            // shader 以 gl_GlobalInvocationID.xy 作为绝对画布坐标，dispatch 覆盖全画布，
            // 由 shader 内的 uv 裁剪只写 stamp 包围盒（§4.5 权威 shader 文本）。
            const uint32_t gx = (uint32_t)std::ceil((double)width / 8.0);
            const uint32_t gy = (uint32_t)std::ceil((double)height / 8.0);
            vkCmdDispatch(commandBuffer, gx, gy, 1);
        }
        SubmitAndWait();
    }

    void ClearCanvasLocked() {
        if (!canvasReady) {
            return;
        }
        BeginCommands();
        VkClearColorValue clear{};
        clear.float32[0] = 1.0f;
        clear.float32[1] = 1.0f;
        clear.float32[2] = 1.0f;
        clear.float32[3] = 1.0f;
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        vkCmdClearColorImage(commandBuffer, canvasImage, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        SubmitAndWait();
    }

    void ReadbackLocked(void* rgbaOut) {
        if (!canvasReady || rgbaOut == nullptr) {
            return;
        }
        BeginCommands();
        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageOffset = {0, 0, 0};
        region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
        vkCmdCopyImageToBuffer(commandBuffer, canvasImage, VK_IMAGE_LAYOUT_GENERAL, readbackBuffer,
                               1, &region);
        SubmitAndWait();
        void* data = nullptr;
        vkMapMemory(device, readbackMemory, 0, readbackSize, 0, &data);
        std::memcpy(rgbaOut, data, (size_t)readbackSize);
        vkUnmapMemory(device, readbackMemory);
    }

    void DestroyDevice() {
        if (device == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(device);
        DestroyCanvas();
        stagingBuffer.reset();
        stagingMemory.reset();
        fence.reset();
        pipeline.reset();
        pipelineLayout.reset();
        descriptorLayout.reset();
        sampler.reset();
        commandPool.reset();
        commandBuffer = VK_NULL_HANDLE;
        device.reset();
        instance.reset();
        deviceReady = false;
    }
};

VkBackend::VkBackend() : impl_(std::make_unique<Impl>()) {}

VkBackend::~VkBackend() {
    shutdown();
}

void VkBackend::init(PlatformSurface surface, int w, int h) {
    if (surface == nullptr) {
        // headless 兼容 dgcSetSurface(NULL, ...)：委托离屏。
        initOffscreen(w, h);
        return;
    }
    // 窗口/swapchain 路径本期不做。
    std::fprintf(stderr, "[VkBackend] windowed surface path not implemented (B2-1)\n");
}

void VkBackend::resize(int w, int h) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->deviceReady) {
        return;
    }
    impl_->CreateCanvas(w, h);
}

void VkBackend::beginFrame() {
    // 离屏无需每帧 begin/end。
}

void VkBackend::composite(const std::vector<StampData>& stamps) {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->CompositeLocked(stamps);
}

void VkBackend::clearCanvas() {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->ClearCanvasLocked();
}

void VkBackend::present() {
    // 离屏模式 no-op（§4.0.5）。
}

void VkBackend::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->DestroyDevice();
}

void VkBackend::initOffscreen(int w, int h) {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->EnsureDevice();
    impl_->CreateCanvas(w, h);
}

void VkBackend::readback(void* rgbaOut) {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->ReadbackLocked(rgbaOut);
}

void VkBackend::exportPNG(const char* path) {
    if (path == nullptr) {
        return;
    }
    std::vector<uint8_t> buf;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!impl_->canvasReady || impl_->width <= 0 || impl_->height <= 0) {
            return;
        }
        buf.resize((size_t)impl_->width * (size_t)impl_->height * 4);
        impl_->ReadbackLocked(buf.data());
    }
    if (stbi_write_png(path, impl_->width, impl_->height, 4, buf.data(),
                       impl_->width * 4) == 0) {
        std::fprintf(stderr, "[VkBackend] stbi_write_png failed for %s\n", path);
    }
}
