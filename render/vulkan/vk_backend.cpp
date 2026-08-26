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

// 与 brush_composite.comp 的 push_constant 布局一一对应（B4-1 融合版）。
// vec3 在 push constant（std140 语义）有 16 字节对齐陷阱，故用 flat float rgb[3]。
struct BrushPushConstant {
    float pos[2];
    float radius;
    float hardness;
    float softness;
    float opacity;
    float rgb[3];
    std::uint32_t shapeType;
};
static_assert(sizeof(BrushPushConstant) == 10 * sizeof(float), "push constant size");

// §4.5 GLSL → SPIR-V：host 用 shaderc 库在代码内编译（不 shell 调 glslc/glslangValidator）；
// Android（NDK 无 libshaderc）改走构建期 glslc 预编译的内嵌 SPIR-V（DGCPAIN_PRECOMPILED_SPV）。
// B4-1：useDerivatives 为真时定义 DGC_USE_FWIDTH，启用 compute 内 fwidth（需设备支持
// VK_NV_compute_shader_derivatives）；否则走解析 AA 宽度兜底（R1）。
std::vector<uint32_t> CompileBrushShader(bool useDerivatives, std::string* err) {
#ifdef DGCPAIN_PRECOMPILED_SPV
    (void)useDerivatives;  // 预编译 SPIR-V 已在构建期按「无 fwidth」路径生成（解析兜底）。
    (void)err;             // 无运行时编译错误路径
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
    if (useDerivatives) {
        options.AddMacroDefinition("DGC_USE_FWIDTH", "1");
    }
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
    VkDeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> descriptorLayout;
    VkDeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout> pipelineLayout;
    VkDeviceHandle<VkPipeline, vkDestroyPipeline> pipeline;

    VkDeviceHandle<VkImage, vkDestroyImage> canvasImage;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> canvasMemory;
    VkDeviceHandle<VkImageView, vkDestroyImageView> canvasView;
    int width = 0;
    int height = 0;

    VkDeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool> descriptorPool;
    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;

    VkDeviceHandle<VkBuffer, vkDestroyBuffer> readbackBuffer;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> readbackMemory;
    VkDeviceSize readbackSize = 0;

    bool useDerivatives = false;  // 设备支持 VK_NV_compute_shader_derivatives → fwidth 路径。
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

        // B4-1：探测 VK_NV_compute_shader_derivatives（compute 内 fwidth 需要）。
        // 支持则启用 fwidth 路径；不支持走解析 AA 宽度兜底（R1）。
        useDerivatives = false;
        {
            uint32_t extCount = 0;
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, nullptr);
            std::vector<VkExtensionProperties> exts(extCount);
            vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extCount, exts.data());
            for (const VkExtensionProperties& e : exts) {
                if (std::strcmp(e.extensionName,
                                VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) == 0) {
                    useDerivatives = true;
                    break;
                }
            }
        }
        std::fprintf(stderr, "[VkBackend] dab raster AA: %s\n",
                     useDerivatives ? "fwidth (compute derivatives)"
                                    : "analytic ww=1/radius fallback");

        float priority = 1.0f;
        VkDeviceQueueCreateInfo queueInfo{};
        queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueInfo.queueFamilyIndex = queueFamily;
        queueInfo.queueCount = 1;
        queueInfo.pQueuePriorities = &priority;

        // fwidth（隐式导数）按 2x2 quad 组计算，需显式开启该 feature。
        VkPhysicalDeviceComputeShaderDerivativesFeaturesNV derivFeatures{};
        derivFeatures.sType =
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COMPUTE_SHADER_DERIVATIVES_FEATURES_NV;
        derivFeatures.computeDerivativeGroupQuads = VK_TRUE;
        const char* derivExtName = VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME;

        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        if (useDerivatives) {
            deviceInfo.enabledExtensionCount = 1;
            deviceInfo.ppEnabledExtensionNames = &derivExtName;
            deviceInfo.pNext = &derivFeatures;
        }
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

        std::array<VkDescriptorSetLayoutBinding, 1> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
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
        std::vector<uint32_t> spv = CompileBrushShader(useDerivatives, &err);
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

    void DestroyCanvas() {
        // descriptorPool 销毁 → 隐式释放 descriptorSet（池子句柄，无独立守卫）。
        descriptorPool.reset();
        descriptorSet = VK_NULL_HANDLE;
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

        std::array<VkDescriptorPoolSize, 1> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 1;
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
            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                                    1, &descriptorSet, 0, nullptr);

            BrushPushConstant pc{};
            pc.pos[0] = s.x;
            pc.pos[1] = s.y;
            pc.radius = s.radius;
            pc.hardness = s.hardness;
            pc.softness = s.softness;
            pc.opacity = s.opacity;
            pc.rgb[0] = s.r;
            pc.rgb[1] = s.g;
            pc.rgb[2] = s.b;
            pc.shapeType = 0u;  // 0=圆形软笔（默认），1/2/3 扩展位（本期不实现）。
            vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(pc), &pc);

            // shader 以 gl_GlobalInvocationID.xy 作为绝对画布坐标，dispatch 覆盖全画布，
            // 由 shader 内的 SDF dist 越界早退（coverage<=0.001 return）只写 dab 覆盖区。
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
        fence.reset();
        pipeline.reset();
        pipelineLayout.reset();
        descriptorLayout.reset();
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
