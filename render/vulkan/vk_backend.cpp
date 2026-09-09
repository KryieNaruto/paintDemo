#include "render/vulkan/vk_backend.h"

#include <vulkan/vulkan.h>

#ifdef DGCPAIN_ANDROID
// A8-5：Android WSI。host 无 android platform 头/符号，须以 DGCPAIN_ANDROID 隔离。
#include <android/native_window.h>  // ANativeWindow（消费端 dgcSetSurface 传入的窗口句柄）
#include <vulkan/vulkan_android.h>  // vkCreateAndroidSurfaceKHR（VK_KHR_android_surface）
#endif

#ifdef DGCPAIN_RENDERDOC_ENABLED
#include "render/renderdoc/renderdoc_capture.h"
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifdef DGCPAIN_PRECOMPILED_SPV
#include "brush_composite_spv.h"  // Android：构建期 glslc 预编译内嵌（单一权威源仍是 .comp）
#include "merge_spv.h"            // A8-2：merge.comp（canvas+tip→display）预编译内嵌
#else
#include <shaderc/shaderc.hpp>
#include "brush_composite_glsl.h"  // 由 CMake 从 brush_composite.comp 生成（单一权威源）
#include "merge_glsl.h"            // A8-2：merge.comp 运行时 shaderc 编译源
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
    float dispatchOffset[2];  // 包围盒 dispatch 原点（shader：c = gl_GlobalInvocationID.xy + offset）
};
static_assert(sizeof(BrushPushConstant) == 12 * sizeof(float), "push constant size");

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
    // 内嵌 OpSource debug 信息：RenderDoc 的 Pipeline State 才能显示 GLSL 源码
    // （画世界式调试视图），否则只有 SPIR-V 反汇编。不改变渲染语义，像素确定性不受影响。
    options.SetGenerateDebugInfo();
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

// A8-2：merge.comp（canvas + tip → displayImage）全画布 over 合成 shader 编译。
// 与 brush_composite 不同：无 derivative/fwidth、无 push constant（fullscreen 用
// gl_GlobalInvocationID 直接当像素坐标 + 普通 vkCmdDispatch，对齐 Mali dispatch-base
// gotcha），故两条路径都只需一次直接编译，无 useDerivatives 分支。
std::vector<uint32_t> CompileMergeShader(std::string* err) {
#ifdef DGCPAIN_PRECOMPILED_SPV
    (void)err;  // 预编译 SPIR-V 无运行时编译错误路径
    const size_t nbytes = sizeof(kMergeSpv);
    static_assert(sizeof(kMergeSpv) % sizeof(uint32_t) == 0,
                  "embedded merge SPIR-V byte array must be 4-byte aligned");
    std::vector<uint32_t> spv(nbytes / sizeof(uint32_t));
    std::memcpy(spv.data(), kMergeSpv, nbytes);
    return spv;
#else
    shaderc::Compiler compiler;
    shaderc::CompileOptions options;
    options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
    options.SetOptimizationLevel(shaderc_optimization_level_performance);
    options.SetGenerateDebugInfo();
    shaderc::SpvCompilationResult result = compiler.CompileGlslToSpv(
        kMergeGlsl, std::strlen(kMergeGlsl),
        shaderc_compute_shader, "merge.comp", options);
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

// 读回 buffer 内存类型选择（优化 4）：优先 HOST_CACHED——Mali 的 HOST_COHERENT
// 是 uncached（读回 3.1MB memcpy ~8.7ms，每字节从 DRAM 取），cacheable 内存让大块
// memcpy 走 CPU 缓存快得多，代价是读前需 vkInvalidateMappedMemoryRanges 使设备写入可见。
// 无 HOST_CACHED 类型则回退调用方原有的 HOST_COHERENT 路径（返回 false，不 invalidate）。
// 返回 true 表示命中了 HOST_CACHED（ReadbackLocked 读前必须 invalidate）。
bool FindReadbackCachedType(VkPhysicalDevice phys, uint32_t typeFilter, uint32_t& outIndex) {
    VkPhysicalDeviceMemoryProperties memProps;
    vkGetPhysicalDeviceMemoryProperties(phys, &memProps);
    for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
        const VkMemoryType& mt = memProps.memoryTypes[i];
        if ((typeFilter & (1u << i)) == 0) continue;
        if ((mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) == 0) continue;
        if (mt.propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) {
            outIndex = i;
            return true;
        }
    }
    return false;
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

// 读回 buffer 专用创建（优化 4）：优先 HOST_CACHED（大块 memcpy 走 CPU 缓存），
// 无则回退原 HOST_VISIBLE|HOST_COHERENT。outCached=true → 读前必须 invalidate。
bool CreateReadbackBuffer(VkDevice device, VkPhysicalDevice phys, VkDeviceSize size,
                          VkBuffer& buffer, VkDeviceMemory& memory, bool& outCached) {
    VkBufferCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &info, nullptr, &buffer) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkCreateBuffer(readback) failed\n");
        return false;
    }
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    uint32_t idx = 0;
    bool cached = FindReadbackCachedType(phys, req.memoryTypeBits, idx);
    if (!cached) {
        // 设备无 HOST_CACHED 类型 → 回退原 HOST_COHERENT 语义（FindMemoryType 找不到返回 0，
        // 与原实现一致，分配失败会走下方清理路径）。
        idx = FindMemoryType(phys, req.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    }
    VkMemoryAllocateInfo alloc{};
    alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc.allocationSize = req.size;
    alloc.memoryTypeIndex = idx;
    if (vkAllocateMemory(device, &alloc, nullptr, &memory) != VK_SUCCESS) {
        std::fprintf(stderr, "[VkBackend] vkAllocateMemory(readback) failed\n");
        vkDestroyBuffer(device, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return false;
    }
    vkBindBufferMemory(device, buffer, memory, 0);
    outCached = cached;
    return true;
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

// A8-5：VkSurfaceKHR 用 vkDestroySurfaceKHR(VkInstance, ...) 销毁，首参是 instance，与
// VkDeviceHandle（首参 VkDevice）不匹配 → 新增 instance-bound 守卫（VK_KHR_surface）。
// 模式完全仿 VkDeviceHandle：值语义、禁拷贝、可移动、reset()/析构幂等，额外持 instance。
template <typename H, void (*Destroy)(VkInstance, H, const VkAllocationCallbacks*)>
struct VkInstanceHandle {
    VkInstance inst = VK_NULL_HANDLE;
    H h = VK_NULL_HANDLE;

    VkInstanceHandle() = default;
    VkInstanceHandle(const VkInstanceHandle&) = delete;
    VkInstanceHandle& operator=(const VkInstanceHandle&) = delete;
    VkInstanceHandle(VkInstanceHandle&& other) noexcept : inst(other.inst), h(other.h) {
        other.h = VK_NULL_HANDLE;
    }
    VkInstanceHandle& operator=(VkInstanceHandle&& other) noexcept {
        if (this != &other) {
            reset();
            inst = other.inst;
            h = other.h;
            other.h = VK_NULL_HANDLE;
        }
        return *this;
    }
    ~VkInstanceHandle() { reset(); }

    H get() const { return h; }
    operator H() const { return h; }

    void reset() {
        if (h != VK_NULL_HANDLE) {
            Destroy(inst, h, nullptr);
            h = VK_NULL_HANDLE;
        }
    }
    H release() {
        H t = h;
        h = VK_NULL_HANDLE;
        return t;
    }
    // 收编新句柄：先销毁旧句柄，再记录 instance 与新句柄。
    void assign(VkInstance i, H handle) {
        reset();
        inst = i;
        h = handle;
    }
};

}  // namespace

struct VkBackend::Impl {
    // 声明序 = instance → device → 全部子对象：逆声明序析构 = 子对象 → device → instance，
    // 即 Vulkan 释放顺序正确。commandBuffer/descriptorSet 属池子句柄，不独立守卫。
    // rdc 声明在 instance 之前 → 逆序析构最后销毁（RenderDoc 库在整个 Vulkan 拆除期间保持加载，
    // 避免若未来启用 layer 注入时 dispatch 表悬空；默认关，未启用时内部 no-op）。
#ifdef DGCPAIN_RENDERDOC_ENABLED
    std::unique_ptr<RenderDocCapture> rdc;
    bool initCaptureOpen_ = false;  // 「资源创建→首次 composite」合并抓帧窗口是否开启
#endif
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
    bool readbackCached_ = false;  // 读回 buffer 用 HOST_CACHED 内存 → 读前须 invalidate

    // ── A8-2 wet-tip 层 ──
    // tipImage：预测 dab 的瞬态 storage（predictor 激活期显示，落笔/clear 清空）。
    // tipDescriptorSet 与 descriptorSet 共用同一 descriptorPool（池句柄，无独立守卫），
    // 只是 binding 0 指向 tipView——复用 brush_composite 管线，内核/shader 零改动。
    VkDeviceHandle<VkImage, vkDestroyImage> tipImage;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> tipMemory;
    VkDeviceHandle<VkImageView, vkDestroyImageView> tipView;
    VkDescriptorSet tipDescriptorSet = VK_NULL_HANDLE;
    bool tipHasContent_ = false;  // 有预测 dab 已画进 tipImage（merge 才需要读它）

    // displayImage：merge（canvas+tip）输出的全画布 storage，读回源（有 tip 时）。
    VkDeviceHandle<VkImage, vkDestroyImage> displayImage;
    VkDeviceHandle<VkDeviceMemory, vkFreeMemory> displayMemory;
    VkDeviceHandle<VkImageView, vkDestroyImageView> displayView;

    // merge 管线（3 张 storage image：canvas/tip/display），无 push constant。
    VkDeviceHandle<VkDescriptorSetLayout, vkDestroyDescriptorSetLayout> mergeDescriptorLayout;
    VkDeviceHandle<VkPipelineLayout, vkDestroyPipelineLayout> mergePipelineLayout;
    VkDeviceHandle<VkPipeline, vkDestroyPipeline> mergePipeline;
    VkDeviceHandle<VkDescriptorPool, vkDestroyDescriptorPool> mergeDescriptorPool;
    VkDescriptorSet mergeDescriptorSet = VK_NULL_HANDLE;

    // ── A8-5 onscreen swapchain（可选上屏路径；离屏仍是唯一权威 canvasImage）──
    // 声明序在 device 之后：逆声明序析构 = swapchain → surface → … → device → instance，
    // 即 Vulkan 要求的孩子先于父释放（surface 只需 instance）。swapchainImages 属 swapchain
    // 所有（image 生命随 swapchain，非自有资源），不逐个 vkDestroyImage、无独立守卫。
    VkInstanceHandle<VkSurfaceKHR, vkDestroySurfaceKHR> surface;
    VkDeviceHandle<VkSwapchainKHR, vkDestroySwapchainKHR> swapchain;
    std::vector<VkImage> swapchainImages;  // 当前 swapchain 的 images（blit 目标池）
    VkExtent2D swapchainExtent{};          // 当前 swapchain 尺寸（blit 目标区域）
    VkPresentModeKHR presentMode_ = VK_PRESENT_MODE_FIFO_KHR;  // 实际选中（MAILBOX 优先退 FIFO）
    bool onscreen_ = false;       // 已绑 surface+swapchain → present() 走 onscreen 分支
    bool swapchainValid_ = false; // 当前 swapchain 可用；out-of-date/surface-lost 置否
#ifdef DGCPAIN_ANDROID
    ANativeWindow* androidWindow_ = nullptr;  // 建 surface 的窗口（同窗复用 / 换窗重建判定）
#endif

    // bugfix（20fps 回退）：readback 快照缓存 —— 渲染线程每次 composite/clear 完成后
    // "顺手"把画布发布进这里；VkBackend::readback() 只从这里 memcpy，不碰 GPU、不等
    // 渲染线程。cache_mutex_ 与上面串行化 GPU 提交的 mutex_（VkBackend 成员）分开，
    // 避免 GUI 线程的读回被渲染线程的 GPU 提交阻塞（见 docs/plans/
    // bugfix-readback-blocks-render-thread.md）。
    std::vector<std::uint8_t> cache_;
    std::mutex cache_mutex_;

    bool useDerivatives = false;  // 设备支持 VK_NV_compute_shader_derivatives → fwidth 路径。
    bool deviceReady = false;
    bool canvasReady = false;

    // Bug #3（快照刷新节流）：消费者请求快照刷新的非阻塞标志。requestSnapshotRefresh()
    // 只 store(true)，不碰 GPU；CompositeLocked 末尾 exchange(false) 消费——已置位才实际
    // 执行 RefreshReadbackCacheLocked()。跨线程安全（engine 渲染线程 vs C API 调用线程），
    // 不参与 GPU 提交串行化（不占用 mutex_ 语义：store/exchange 均无锁）。
    std::atomic<bool> snapshotRefreshRequested_{false};

#ifdef DGCPAIN_TEST_HOOKS
    // Bug3（dab 合成孔洞）回归：CompositeLocked 里实际 dispatch / barrier 的调用计数，
    // 供 test_composite_barrier_repro 断言「每次 dispatch 后都插了一次 image barrier」。
    // 仅测试编译（顶层 CMake 选项 DGCPAIN_TEST_HOOKS），生产构建零开销。
    std::uint64_t dispatchCount_ = 0;
    std::uint64_t barrierCount_ = 0;

    // Bug #3（快照刷新节流）回归计数：
    //   compositeCount_      = 每次非空 composite 批提交 +1（CompositeLocked 通过空检查后）。
    //   snapshotRefreshCount_= 每次实际全画布 GPU→CPU 快照拷贝（RefreshReadbackCacheLocked）
    //                          +1，含 CompositeLocked 末尾与 ClearCanvasLocked 的刷新。
    // 修复前（红）：每次 composite 都无条件刷新 → snapshotRefreshCount ≈ compositeCount。
    // 修复后（绿）：仅在消费者请求/结算时才刷新 → snapshotRefreshCount ≪ compositeCount。
    std::uint64_t snapshotRefreshCount_ = 0;
    std::uint64_t compositeCount_ = 0;
    // 4a 回归：SubmitAndWait() 实际调用次数（每次 GPU 提交+等 fence +1）。修复前每次
    // composite-with-refresh 会付两次（composite 自己一次 + RefreshReadbackCacheLocked
    // 自己一次）；修复后合并成一次（composite 触发的刷新不再额外付提交，仅 Clear*/
    // flushReadbackCache 独立调用点仍各付一次）。
    std::uint64_t submitAndWaitCount_ = 0;
#endif

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

        // 离屏：无 layer。A8-5：Android 编译期无条件 enable WSI surface 扩展（.so 纯
        // Android 构建，无副作用；host/headless 不 enable，present 保持 no-op）。
        VkInstanceCreateInfo instanceInfo{};
        instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        instanceInfo.pApplicationInfo = &appInfo;
#ifdef DGCPAIN_ANDROID
        const char* instExtNames[] = {VK_KHR_SURFACE_EXTENSION_NAME,
                                      VK_KHR_ANDROID_SURFACE_EXTENSION_NAME};
        instanceInfo.enabledExtensionCount =
            (uint32_t)(sizeof(instExtNames) / sizeof(instExtNames[0]));
        instanceInfo.ppEnabledExtensionNames = instExtNames;
#endif
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

        // 设备扩展：B4-1 fwidth（若支持）+ A8-5 Android VK_KHR_swapchain（编译期无条件，
        // 与实例级 surface 扩展配套；host/headless 不 enable）。
        std::array<const char*, 2> devExtNames{};
        uint32_t devExtCount = 0;
        if (useDerivatives) {
            devExtNames[devExtCount++] = derivExtName;
        }
#ifdef DGCPAIN_ANDROID
        devExtNames[devExtCount++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
#endif
        VkDeviceCreateInfo deviceInfo{};
        deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        deviceInfo.queueCreateInfoCount = 1;
        deviceInfo.pQueueCreateInfos = &queueInfo;
        deviceInfo.enabledExtensionCount = devExtCount;
        deviceInfo.ppEnabledExtensionNames = devExtCount ? devExtNames.data() : nullptr;
        if (useDerivatives) {
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

#ifdef DGCPAIN_RENDERDOC_ENABLED
        // B5-4 fix：构造并开启「资源创建 + 首次 composite」合并抓帧。必须在管线/描述符/画布
        // 创建之前 StartFrameCapture——否则这些对象在抓帧窗口之外创建，RenderDoc 无法解析
        // dispatch 的管线状态（Pipeline State 各面板全空）。首次 composite 的 EndFrameCapture
        // 收尾此条完整 capture（见 CompositeLocked）。未启用/加载失败内部降级 no-op。
        // 注意：必须在 vkCreateDevice 之后调用——RenderDoc 需从 instance 解析出 VkDevice
        // 上下文，VkDevice 存在之前 StartFrameCapture 会直接崩溃（RenderDoc launch 注入复现）。
        rdc = std::make_unique<RenderDocCapture>();
        rdc->startFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance.get()));
        initCaptureOpen_ = rdc->available();
#endif

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

        // ── A8-2 merge 管线（canvas + tip → displayImage）fullscreen over 合成 ──
        // 无 push constant：merge.comp 用 gl_GlobalInvocationID 直接当像素坐标 + 普通
        // vkCmdDispatch（对齐 Mali dispatch-base gotcha，见 merge.comp 注释）。
        {
            std::array<VkDescriptorSetLayoutBinding, 3> mergeBindings{};
            mergeBindings[0].binding = 0;
            mergeBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            mergeBindings[0].descriptorCount = 1;
            mergeBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            mergeBindings[1].binding = 1;
            mergeBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            mergeBindings[1].descriptorCount = 1;
            mergeBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            mergeBindings[2].binding = 2;
            mergeBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            mergeBindings[2].descriptorCount = 1;
            mergeBindings[2].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            VkDescriptorSetLayoutCreateInfo mlayoutInfo{};
            mlayoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
            mlayoutInfo.bindingCount = (uint32_t)mergeBindings.size();
            mlayoutInfo.pBindings = mergeBindings.data();
            VkDescriptorSetLayout mdsl = VK_NULL_HANDLE;
            vkCreateDescriptorSetLayout(device, &mlayoutInfo, nullptr, &mdsl);
            mergeDescriptorLayout.assign(device, mdsl);

            VkPipelineLayoutCreateInfo mplInfo{};
            mplInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            mplInfo.setLayoutCount = 1;
            VkDescriptorSetLayout msetLayouts[] = {mergeDescriptorLayout};
            mplInfo.pSetLayouts = msetLayouts;
            mplInfo.pushConstantRangeCount = 0;
            mplInfo.pPushConstantRanges = nullptr;
            VkPipelineLayout mpl = VK_NULL_HANDLE;
            vkCreatePipelineLayout(device, &mplInfo, nullptr, &mpl);
            mergePipelineLayout.assign(device, mpl);

            std::string merr;
            std::vector<uint32_t> mspv = CompileMergeShader(&merr);
            if (mspv.empty()) {
                std::fprintf(stderr, "[VkBackend] merge shaderc compile failed: %s\n", merr.c_str());
                return;
            }
            VkShaderModuleCreateInfo mmoduleInfo{};
            mmoduleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
            mmoduleInfo.codeSize = mspv.size() * sizeof(uint32_t);
            mmoduleInfo.pCode = mspv.data();
            VkShaderModule mmodule = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device, &mmoduleInfo, nullptr, &mmodule) != VK_SUCCESS) {
                std::fprintf(stderr, "[VkBackend] vkCreateShaderModule(merge) failed\n");
                return;
            }
            VkComputePipelineCreateInfo mpipeInfo{};
            mpipeInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            mpipeInfo.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            mpipeInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            mpipeInfo.stage.module = mmodule;
            mpipeInfo.stage.pName = "main";
            mpipeInfo.layout = mergePipelineLayout;
            VkPipeline mpipe = VK_NULL_HANDLE;
            if (vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &mpipeInfo, nullptr, &mpipe) !=
                VK_SUCCESS) {
                std::fprintf(stderr, "[VkBackend] vkCreateComputePipelines(merge) failed\n");
            }
            mergePipeline.assign(device, mpipe);
            vkDestroyShaderModule(device, mmodule, nullptr);
        }

        deviceReady = true;
    }

    void DestroyCanvas() {
        // descriptorPool 销毁 → 隐式释放 descriptorSet / tipDescriptorSet（池子句柄，无独立守卫）。
        descriptorPool.reset();
        descriptorSet = VK_NULL_HANDLE;
        canvasView.reset();
        canvasImage.reset();
        canvasMemory.reset();
        readbackBuffer.reset();
        readbackMemory.reset();
        // A8-2：tip/display 资源 + merge descriptor pool 随画布一起重建/销毁。
        tipDescriptorSet = VK_NULL_HANDLE;
        tipView.reset();
        tipImage.reset();
        tipMemory.reset();
        tipHasContent_ = false;
        displayView.reset();
        displayImage.reset();
        displayMemory.reset();
        mergeDescriptorPool.reset();
        mergeDescriptorSet = VK_NULL_HANDLE;
        width = height = 0;
        readbackSize = 0;
        readbackCached_ = false;
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

        // A8-2 tip 层：预测 dab 的瞬态 storage（同格式同尺寸）。需要 STORAGE（brush_composite
        // 写）+ SAMPLED（merge 读）+ TRANSFER_DST（vkCmdClearColorImage 清）。
        VkImage tipImg = VK_NULL_HANDLE;
        VkDeviceMemory tipMem = VK_NULL_HANDLE;
        CreateImage(device, physicalDevice, (uint32_t)w, (uint32_t)h, kCanvasFormat,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                        VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, tipImg, tipMem);
        tipImage.assign(device, tipImg);
        tipMemory.assign(device, tipMem);
        tipView.assign(device, CreateImageView(device, tipImg, kCanvasFormat));
        if (tipImg == VK_NULL_HANDLE || tipView == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[VkBackend] tip layer creation failed\n");
            return;
        }

        // A8-2 display 层：merge（canvas+tip）输出的全画布 storage，读回源。
        // STORAGE（merge 写）+ TRANSFER_SRC（CopyImageToBuffer 读）。
        VkImage dispImg = VK_NULL_HANDLE;
        VkDeviceMemory dispMem = VK_NULL_HANDLE;
        CreateImage(device, physicalDevice, (uint32_t)w, (uint32_t)h, kCanvasFormat,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    VK_IMAGE_LAYOUT_GENERAL, dispImg, dispMem);
        displayImage.assign(device, dispImg);
        displayMemory.assign(device, dispMem);
        displayView.assign(device, CreateImageView(device, dispImg, kCanvasFormat));
        if (dispImg == VK_NULL_HANDLE || displayView == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[VkBackend] display image creation failed\n");
            return;
        }

        readbackSize = (VkDeviceSize)w * (VkDeviceSize)h * 4;
        VkBuffer rbuf = VK_NULL_HANDLE;
        VkDeviceMemory rmem = VK_NULL_HANDLE;
        // 优化 4：优先 HOST_CACHED（Mali uncached COHERENT memcpy ~8.7ms → cacheable 快得多），
        // 无则回退 HOST_COHERENT。readbackCached_=true 时 ReadbackLocked 读前须 invalidate。
        bool rbCached = false;
        bool rbOk = CreateReadbackBuffer(device, physicalDevice, readbackSize, rbuf, rmem, rbCached);
        readbackCached_ = rbOk && rbCached;  // 仅成功创建且命中 HOST_CACHED 才 invalidate
        readbackBuffer.assign(device, rbuf);
        readbackMemory.assign(device, rmem);
        if (rbuf == VK_NULL_HANDLE) {
            std::fprintf(stderr, "[VkBackend] readback buffer creation failed\n");
            return;
        }

        std::array<VkDescriptorPoolSize, 1> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSizes[0].descriptorCount = 2;  // A8-2：canvas + tip 两个 storage image 描述符
        VkDescriptorPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        poolInfo.maxSets = 2;
        poolInfo.poolSizeCount = (uint32_t)poolSizes.size();
        poolInfo.pPoolSizes = poolSizes.data();
        VkDescriptorPool dpool = VK_NULL_HANDLE;
        vkCreateDescriptorPool(device, &poolInfo, nullptr, &dpool);
        descriptorPool.assign(device, dpool);

        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = 2;
        VkDescriptorSetLayout setLayouts[] = {descriptorLayout, descriptorLayout};
        allocInfo.pSetLayouts = setLayouts;
        VkDescriptorSet sets[2] = {VK_NULL_HANDLE, VK_NULL_HANDLE};
        vkAllocateDescriptorSets(device, &allocInfo, sets);
        descriptorSet = sets[0];
        tipDescriptorSet = sets[1];

        VkDescriptorImageInfo canvasInfo{};
        canvasInfo.imageView = canvasView;
        canvasInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet canvasWrite{};
        canvasWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        canvasWrite.dstSet = descriptorSet;
        canvasWrite.dstBinding = 0;
        canvasWrite.descriptorCount = 1;
        canvasWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        canvasWrite.pImageInfo = &canvasInfo;

        VkDescriptorImageInfo tipInfo{};
        tipInfo.imageView = tipView;
        tipInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
        VkWriteDescriptorSet tipWrite{};
        tipWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        tipWrite.dstSet = tipDescriptorSet;
        tipWrite.dstBinding = 0;
        tipWrite.descriptorCount = 1;
        tipWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        tipWrite.pImageInfo = &tipInfo;

        VkWriteDescriptorSet writes[2] = {canvasWrite, tipWrite};
        vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);

        // A8-2 merge descriptor set：binding 0=canvas、1=tip、2=display。
        {
            std::array<VkDescriptorPoolSize, 1> mergePoolSizes{};
            mergePoolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            mergePoolSizes[0].descriptorCount = 3;
            VkDescriptorPoolCreateInfo mpoolInfo{};
            mpoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
            mpoolInfo.maxSets = 1;
            mpoolInfo.poolSizeCount = (uint32_t)mergePoolSizes.size();
            mpoolInfo.pPoolSizes = mergePoolSizes.data();
            VkDescriptorPool mpool = VK_NULL_HANDLE;
            vkCreateDescriptorPool(device, &mpoolInfo, nullptr, &mpool);
            mergeDescriptorPool.assign(device, mpool);

            VkDescriptorSetAllocateInfo mAlloc{};
            mAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
            mAlloc.descriptorPool = mergeDescriptorPool;
            mAlloc.descriptorSetCount = 1;
            VkDescriptorSetLayout msetLayouts[] = {mergeDescriptorLayout};
            mAlloc.pSetLayouts = msetLayouts;
            vkAllocateDescriptorSets(device, &mAlloc, &mergeDescriptorSet);

            VkDescriptorImageInfo mInfos[3]{};
            mInfos[0].imageView = canvasView;
            mInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            mInfos[1].imageView = tipView;
            mInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            mInfos[2].imageView = displayView;
            mInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            VkWriteDescriptorSet mWrites[3]{};
            for (int i = 0; i < 3; ++i) {
                mWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                mWrites[i].dstSet = mergeDescriptorSet;
                mWrites[i].dstBinding = (uint32_t)i;
                mWrites[i].descriptorCount = 1;
                mWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                mWrites[i].pImageInfo = &mInfos[i];
            }
            vkUpdateDescriptorSets(device, 3, mWrites, 0, nullptr);
        }

        canvasReady = true;

        // bugfix（20fps 回退）：cache_ 按画布尺寸预置（图像内容此刻未定义——按 Vulkan
        // 规范新建 image 内容未初始化，零填充是确定性的安全默认；真实内容由调用方后续
        // clearCanvas()/composite() 触发 RefreshReadbackCacheLocked() 覆盖，与既有调用
        // 约定一致：现有全部调用方均先 dgcSetOffscreenSurface 后 dgcClear 才读回）。
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.assign((size_t)readbackSize, 0);
        }
    }

    void SubmitAndWait() {
#ifdef DGCPAIN_TEST_HOOKS
        ++submitAndWaitCount_;
#endif
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

    void CompositeLocked(const std::vector<StampData>& stamps, bool predicted) {
        if (!canvasReady) {
            return;
        }
        if (stamps.empty()) {
            return;
        }
#ifdef DGCPAIN_TEST_HOOKS
        ++compositeCount_;  // 通过空检查 = 一次实际 composite 批提交（test hook，仅测试构建）。
#endif
        // B5-4：RenderDoc 程序化抓帧 —— Start/End 包住整段 composite commandBuffer
        // （bind pipeline/descriptor/push constants/vkCmdDispatch + 提交完成），与 dispatch
        // 同线程（render 线程）且被 mutex_ 串行。Vulkan 的 RenderDoc device pointer 是
        // VkInstance 的 dispatch 表指针（不能用 VkDevice），故经宏转换后传入。
#ifdef DGCPAIN_RENDERDOC_ENABLED
        // 首次 composite：initCaptureOpen_ 为真（StartFrameCapture 已在资源创建前开启），
        // 不重复 start，由下面 EndFrameCapture 收尾那条完整的「资源创建+首次 composite」抓帧；
        // 后续 composite 各自 start/end（窗口窄，管线状态以首条 capture 为准）。
        void* rdocDevice = rdc ? RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance.get()) : nullptr;
        if (rdc && !initCaptureOpen_) {
            rdc->startFrameCapture(rdocDevice);
        }
#endif
#ifdef DGCPAIN_PERF
        auto t0 = std::chrono::steady_clock::now();
#endif
        BeginCommands();
        // 每 stamp 只变 push constant；pipeline/descriptor 绑定提出循环（避免逐 dab 重绑）。
        // A8-2：predicted 批绑定 tip 描述符（binding 0 指向 tipView），复用同一 brush 管线。
        const VkDescriptorSet ds = predicted ? tipDescriptorSet : descriptorSet;
        VkImage targetImage = predicted ? tipImage.get() : canvasImage.get();
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0,
                                1, &ds, 0, nullptr);
        if (predicted) {
            // 预测批每次重推前先清 tipImage（旧预测尖作废），再画当批。clear 是 transfer 写、
            // 后续 dispatch 是 shader 读，同 command buffer 内需一次 barrier。
            VkClearColorValue clear{};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            vkCmdClearColorImage(commandBuffer, tipImage, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                 &range);
            VkImageMemoryBarrier clearBarrier{};
            clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            clearBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            clearBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            clearBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            clearBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            clearBarrier.image = tipImage;
            clearBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr,
                                 1, &clearBarrier);
        }
        size_t dispatches = 0;
        for (const StampData& s : stamps) {
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

            // 包围盒 dispatch（性能根因二：每 dab 全画布 dispatch 浪费 ~30 倍线程）。
            // shader 写范围硬边界 = 圆心 ± radius（coverage 在 dist>=1.0 分支为 0），
            // 加 1px AA 余量；dispatchOffset push constant 把 gl_GlobalInvocationID 平移到
            // 覆盖区原点，普通 vkCmdDispatch 即可（不用 vkCmdDispatchBase——Mali 驱动实测
            // gl_GlobalInvocationID 未含 base，画不到偏移区域）。覆盖区仍含 dab 全部有效
            // 像素，shader 内 SDF 早退保证输出与全画布逐像素一致。
            const float margin = 1.0f;
            const float x0 = s.x - s.radius - margin;
            const float y0 = s.y - s.radius - margin;
            const float x1 = s.x + s.radius + margin;
            const float y1 = s.y + s.radius + margin;
            const int ix0 = std::max(0, (int)std::floor(x0));
            const int iy0 = std::max(0, (int)std::floor(y0));
            const int ix1 = std::min(width - 1, (int)std::ceil(x1));
            const int iy1 = std::min(height - 1, (int)std::ceil(y1));
            if (ix0 > ix1 || iy0 > iy1) {
                continue;  // dab 完全在画布外：无需 dispatch。
            }
            pc.dispatchOffset[0] = (float)ix0;
            pc.dispatchOffset[1] = (float)iy0;
            const uint32_t cntX = (uint32_t)((ix1 - ix0 + 8) / 8);  // ceil((ix1-ix0+1)/8)
            const uint32_t cntY = (uint32_t)((iy1 - iy0 + 8) / 8);
            vkCmdPushConstants(commandBuffer, pipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                               sizeof(pc), &pc);
            vkCmdDispatch(commandBuffer, cntX, cntY, 1);
            ++dispatches;
#ifdef DGCPAIN_TEST_HOOKS
            ++dispatchCount_;
#endif

            // 修复"dab 孔洞"：相邻 dab 包围盒/覆盖区域几乎必然重叠（间距 2.5px << 半径 10px），
            // 下一次 dispatch 的 shader 对同一批像素 imageLoad 必须保证读到这次 dispatch 刚
            // imageStore 的结果。没有显式 barrier 时，Vulkan 规范不保证同一 command buffer 内
            // 连续两次 dispatch 对同一张 storage image 重叠区域的读写可见性（未定义行为），
            // 部分驱动会因乱序/并行调度读到脏值 → 局部覆盖丢失（用户 Windows 真机 RenderDoc
            // 抓帧截图复现的孔洞）。compute-to-compute 同 stage 的最小化 barrier，只刷新缓存
            // 级别开销，不牵扯图形管线阶段。
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = targetImage;
            barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                 VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                                 0, nullptr, 0, nullptr, 1, &barrier);
#ifdef DGCPAIN_TEST_HOOKS
            ++barrierCount_;
#endif
        }
        // A8-2：预测批 dispatch 完成后置位 tipHasContent_（挪到 SubmitAndWait() 之前，
        // 4a 合并提交需要——若本批同时命中刷新标志，下面 RecordRefreshCommands() 得读到
        // 「这一批」刚置好的 tipHasContent_，不能是 SubmitAndWait 之后才置）。真实批不
        // 置位（tip 内容不变）。
        if (predicted) {
            tipHasContent_ = true;
        }
        // 4a：把读回刷新（merge+copy）合进这次 composite 已经打开的 command buffer，
        // 只 SubmitAndWait() 一次，不再像修复前那样「composite 自己提交一次 + 刷新
        // 又单独提交一次」（真机 Mali 实测每次独立提交固定开销约 2-3ms，见
        // docs/superpowers/specs/2026-09-04-mode-a-ink-parity-design.md §1/§4a）。
        // Bug #3（快照刷新节流）语义不变：仍是「仅当消费者请求过才刷新」，只是刷新的
        // GPU 提交现在跟 dab 合成共享同一次 submit。
        const bool doRefresh =
            snapshotRefreshRequested_.exchange(false, std::memory_order_acq_rel);
        if (doRefresh) {
            RecordRefreshCommands();
        }
        SubmitAndWait();
#ifdef DGCPAIN_PERF
        auto t1 = std::chrono::steady_clock::now();
        std::fprintf(stderr,
                     "[PERF] composite stamps=%zu dispatches=%zu total=%.3f ms (%dx%d)\n",
                     stamps.size(), dispatches,
                     std::chrono::duration<double, std::milli>(t1 - t0).count(), width, height);
#endif
#ifdef DGCPAIN_RENDERDOC_ENABLED
        if (rdc) {
            rdc->endFrameCapture(rdocDevice);
            initCaptureOpen_ = false;
        }
#endif
        if (doRefresh) {
            FinishRefreshReadback();
        }
    }

    void ClearCanvasLocked(float r, float g, float b, float a) {
        if (!canvasReady) {
            return;
        }
        BeginCommands();
        VkClearColorValue clear{};
        clear.float32[0] = r;
        clear.float32[1] = g;
        clear.float32[2] = b;
        clear.float32[3] = a;
        VkImageSubresourceRange range{};
        range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        range.baseMipLevel = 0;
        range.levelCount = 1;
        range.baseArrayLayer = 0;
        range.layerCount = 1;
        vkCmdClearColorImage(commandBuffer, canvasImage, VK_IMAGE_LAYOUT_GENERAL, &clear, 1, &range);
        // A8-2：dgcClear 同清 tip 层（预测尖一并丢弃）。
        VkClearColorValue tipClear{};
        vkCmdClearColorImage(commandBuffer, tipImage, VK_IMAGE_LAYOUT_GENERAL, &tipClear, 1,
                             &range);
        SubmitAndWait();
        tipHasContent_ = false;
        // 清屏后画布内容立即变化，快照缓存必须同步刷新，否则 readback() 在下一次
        // composite 之前会一直读到清屏前的旧内容。
        RefreshReadbackCacheLocked();
    }

    // A8-2：endStroke 清 tip（丢弃预测尖）。渲染线程 FIFO 顺序保证在所有预测批 composite
    // 之后才被调（engine clearTip 批）。清完刷新读回快照，使落笔后显示立即回到纯真实墨。
    void ClearTipLocked() {
        if (!canvasReady) {
            return;
        }
        const bool hadTip = tipHasContent_;
        if (hadTip) {
            BeginCommands();
            VkClearColorValue clear{};
            VkImageSubresourceRange range{};
            range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            range.baseMipLevel = 0;
            range.levelCount = 1;
            range.baseArrayLayer = 0;
            range.layerCount = 1;
            vkCmdClearColorImage(commandBuffer, tipImage, VK_IMAGE_LAYOUT_GENERAL, &clear, 1,
                                 &range);
            SubmitAndWait();
        }
        tipHasContent_ = false;
        // 落笔后 tip 消失，读回快照必须同步回纯真实（否则 cache_ 仍残留 merge 过的 tip）。
        // 仅当此前确有 tip 才付这次 GPU 拷贝；prediction OFF 路径（tipHasContent_ 恒 false）
        // 零额外开销，不改变 test_snapshot_refresh_throttle 的刷新计数。
        if (hadTip) {
            RefreshReadbackCacheLocked();
        }
    }

    void ReadbackLocked(void* rgbaOut) {
        if (!canvasReady || rgbaOut == nullptr) {
            return;
        }
#ifdef DGCPAIN_PERF
        auto t0 = std::chrono::steady_clock::now();
#endif
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
#ifdef DGCPAIN_PERF
        auto t1 = std::chrono::steady_clock::now();
#endif
        void* data = nullptr;
        vkMapMemory(device, readbackMemory, 0, readbackSize, 0, &data);
        if (readbackCached_) {
            // HOST_CACHED：设备写入可能尚未进 CPU 缓存，invalidate 强制丢弃失效缓存行，
            // 让拷贝结果对 host 可见后再 memcpy（COHERENT 内存上 invalidate 无害，故无需分支）。
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readbackMemory.get();
            range.offset = 0;
            range.size = readbackSize;
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        std::memcpy(rgbaOut, data, (size_t)readbackSize);
        vkUnmapMemory(device, readbackMemory);
#ifdef DGCPAIN_PERF
        auto t2 = std::chrono::steady_clock::now();
        std::fprintf(stderr,
                     "[PERF] readback copy+wait=%.3f ms memcpy=%.3f ms (%dx%d %.1f MB)\n",
                     std::chrono::duration<double, std::milli>(t1 - t0).count(),
                     std::chrono::duration<double, std::milli>(t2 - t1).count(), width, height,
                     (double)readbackSize / 1e6);
#endif
    }

    // bugfix（20fps 回退）：把画布发布进 readback 快照缓存——复用与 ReadbackLocked 相同的
    // GPU 拷贝路径，但目标是内部 cache_（配独立 cache_mutex_），不是调用方传入的指针。
    // 调用方（CompositeLocked/ClearCanvasLocked/initOffscreen）均已持有外层 mutex_（串行
    // 化 GPU 提交），此处只需保证 cache_ 本身的写入对 VkBackend::readback() 线程安全可见——
    // cache_mutex_ 临界区仅一次 memcpy，不含任何 GPU 等待，不会把这个等待传导给 GUI 线程。
    // 4a：录制读回刷新的 GPU 命令（merge dispatch 条件性 + copy-to-buffer）到**当前已打开**
    // 的 command buffer；调用方必须已调过 BeginCommands()，本函数不调 SubmitAndWait()——
    // 可以是独立一次提交的一部分（RefreshReadbackCacheLocked 单独调用场景），也可以是
    // CompositeLocked 自己那次提交的一部分（合并省一次 GPU 往返）。
    // A8-2/A8-5 共用的「显示源选择」：present 与读回 refresh 同一口径。仅当 tipHasContent_
    // 为真时，先 fullscreen merge（canvas+tip → displayImage），并插一次 merge shader write →
    // transfer read 的屏障（copy/blit 均为 transfer read）；返回应作为显示/读回源的 image。
    // 无 tip 时直接返回 canvasImage（零额外 GPU 开销）。调用方已 BeginCommands()；返回的
    // image 仍处 GENERAL（blit 的 src 布局过渡由 present 侧自行处理）。
    // 抽成单一实现供读回与 present 复用，避免同一 merge 约定写成两份（§10.6）。
    VkImage RecordDisplaySourceMergeLocked() {
        if (!tipHasContent_) {
            return canvasImage.get();
        }
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, mergePipeline);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                mergePipelineLayout, 0, 1, &mergeDescriptorSet, 0, nullptr);
        vkCmdDispatch(commandBuffer, (uint32_t)((width + 7) / 8),
                      (uint32_t)((height + 7) / 8), 1);
        // merge 写 displayImage（shader write）→ 后续 transfer read（copy/blit）。
        VkImageMemoryBarrier mergeBarrier{};
        mergeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        mergeBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        mergeBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        mergeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        mergeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        mergeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mergeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        mergeBarrier.image = displayImage;
        mergeBarrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &mergeBarrier);
        return displayImage.get();
    }

    void RecordRefreshCommands() {
#ifdef DGCPAIN_TEST_HOOKS
        ++snapshotRefreshCount_;  // 每次实际快照刷新 +1（test hook，仅测试构建）。
#endif
        // A8-2：有 tip 时先 fullscreen merge（canvas+tip → displayImage），读回源改为
        // displayImage；无 tip 时读回源仍为 canvasImage（与改造前逐位一致、零额外 GPU 拷贝）。
        // 源选择逻辑在 RecordDisplaySourceMergeLocked（与 A8-5 present 同口径，单一实现）。
        VkImage srcImage = RecordDisplaySourceMergeLocked();
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
        vkCmdCopyImageToBuffer(commandBuffer, srcImage, VK_IMAGE_LAYOUT_GENERAL, readbackBuffer,
                               1, &region);
    }

    // 4a：读回刷新的提交后 CPU 处理（map/invalidate/memcpy/unmap 进 cache_）。调用方必须
    // 已在 RecordRefreshCommands() 之后调过 SubmitAndWait()（即 GPU 已完成拷贝）。
    void FinishRefreshReadback() {
        void* data = nullptr;
        vkMapMemory(device, readbackMemory, 0, readbackSize, 0, &data);
        if (readbackCached_) {
            VkMappedMemoryRange range{};
            range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
            range.memory = readbackMemory.get();
            range.offset = 0;
            range.size = readbackSize;
            vkInvalidateMappedMemoryRanges(device, 1, &range);
        }
        {
            std::lock_guard<std::mutex> lock(cache_mutex_);
            cache_.resize((size_t)readbackSize);
            std::memcpy(cache_.data(), data, (size_t)readbackSize);
        }
        vkUnmapMemory(device, readbackMemory);
    }

    // 独立调用点（ClearCanvasLocked/ClearTipLocked）保持原有语义：自己开一次完整的
    // Begin→Record→Submit→Finish，不与任何 composite 合并（这两处频率低，不是本次优化目标）。
    void RefreshReadbackCacheLocked() {
        if (!canvasReady) {
            return;
        }
        BeginCommands();
        RecordRefreshCommands();
        SubmitAndWait();
        FinishRefreshReadback();
    }

    // ── A8-5 onscreen swapchain 辅助 ──

    // 拆除 surface+swapchain（幂等）。swapchain 先于 surface 释放，均先于 device/instance
    // （DestroyDevice 于 device.reset() 前调用）。切回离屏（dgcSetSurface(NULL)）/换窗/销毁
    // 竞态兜底时调用。live swapchain 存在时先 vkDeviceWaitIdle，确保无 in-flight acquire/
    // present 引用将销毁的 images。
    void TeardownSwapchainLocked() {
        if (swapchain != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);
        }
        swapchain.reset();
        swapchainImages.clear();
        swapchainExtent = {};
        swapchainValid_ = false;
        onscreen_ = false;
        surface.reset();
#ifdef DGCPAIN_ANDROID
        androidWindow_ = nullptr;
#endif
    }

    // 从 ANativeWindow* 建 VkSurfaceKHR（仅 Android）。同窗已有 surface → 复用（resize/重建
    // swapchain 时 surface 无需重建）；窗口变了 → 先拆旧 surface+swapchain 再建新 surface。
    // 返回 false = 建 surface 失败（走既有错误路径：fprintf + 状态判定，不抛半初始化句柄）。
#ifdef DGCPAIN_ANDROID
    bool SetupAndroidSurfaceLocked(ANativeWindow* window) {
        if (surface != VK_NULL_HANDLE && androidWindow_ == window) {
            return true;
        }
        TeardownSwapchainLocked();
        VkAndroidSurfaceCreateInfoKHR ci{};
        ci.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
        ci.window = window;
        VkSurfaceKHR surf = VK_NULL_HANDLE;
        if (vkCreateAndroidSurfaceKHR(instance.get(), &ci, nullptr, &surf) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateAndroidSurfaceKHR failed\n");
            return false;
        }
        surface.assign(instance.get(), surf);
        androidWindow_ = window;
        return true;
    }
#endif

    // 查询 surface 能力并建 swapchain（present mode 优先 MAILBOX 退 FIFO，记录实际选中）。
    // 重建（resize/out-of-date）时先释放旧 swapchain。失败走错误路径：onscreen_/swapchainValid_
    // 保持 false → present 维持 no-op，不崩。swapchain image 以 TRANSFER_DST 建（blit 目标）。
    void CreateSwapchainLocked() {
        if (!deviceReady || surface == VK_NULL_HANDLE) {
            return;
        }
        if (swapchain != VK_NULL_HANDLE) {
            vkDeviceWaitIdle(device);  // 旧 swapchain 的 in-flight present 先落地
            swapchain.reset();
            swapchainImages.clear();
        }
        swapchainValid_ = false;
        onscreen_ = false;

        VkSurfaceCapabilitiesKHR caps{};
        if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, surface.get(), &caps) !=
            VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkGetPhysicalDeviceSurfaceCapabilitiesKHR failed\n");
            return;
        }
        uint32_t fmtCount = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface.get(), &fmtCount, nullptr);
        std::vector<VkSurfaceFormatKHR> formats(fmtCount);
        if (fmtCount) {
            vkGetPhysicalDeviceSurfaceFormatsKHR(physicalDevice, surface.get(), &fmtCount,
                                                 formats.data());
        }
        VkSurfaceFormatKHR format =
            formats.empty() ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                                 VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
                            : formats[0];
        for (const VkSurfaceFormatKHR& f : formats) {
            if (f.format == kCanvasFormat) {  // 尽量与离屏 canvas 同格式，blit 免格式转换
                format = f;
                break;
            }
        }
        uint32_t pmCount = 0;
        vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface.get(), &pmCount,
                                                  nullptr);
        std::vector<VkPresentModeKHR> modes(pmCount);
        if (pmCount) {
            vkGetPhysicalDeviceSurfacePresentModesKHR(physicalDevice, surface.get(), &pmCount,
                                                      modes.data());
        }
        VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
        for (const VkPresentModeKHR m : modes) {
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) {  // 优先 MAILBOX（低延迟），不支持退 FIFO
                mode = m;
                break;
            }
        }
        presentMode_ = mode;
        std::fprintf(stderr, "[VkBackend] present mode: %s\n",
                     (mode == VK_PRESENT_MODE_MAILBOX_KHR) ? "MAILBOX" : "FIFO");

        VkExtent2D extent = caps.currentExtent;
        if (extent.width == 0xFFFFFFFFu || extent.width == 0 || extent.height == 0xFFFFFFFFu ||
            extent.height == 0) {
            // 非固定尺寸（或暂时为 0，如最小化）：以离屏 canvas 尺寸为期望，夹到 caps 的
            // min/max（无旋转前提下 canvas 与屏幕 1:1，blit 免缩放）。
            extent.width = std::clamp((uint32_t)width, caps.minImageExtent.width,
                                      caps.maxImageExtent.width);
            extent.height = std::clamp((uint32_t)height, caps.minImageExtent.height,
                                       caps.maxImageExtent.height);
        }
        swapchainExtent = extent;

        uint32_t imageCount = caps.minImageCount + 1;  // 尽量 double/triple buffer
        if (caps.maxImageCount > 0 && imageCount > caps.maxImageCount) {
            imageCount = caps.maxImageCount;
        }
        // blit 目标需 TRANSFER_DST；不支持则如实记录，不强行走本路径。
        const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if ((caps.supportedUsageFlags & usage) == 0) {
            std::fprintf(stderr, "[VkBackend] surface lacks TRANSFER_DST for blit present\n");
            return;
        }

        VkSwapchainCreateInfoKHR sci{};
        sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
        sci.surface = surface.get();
        sci.minImageCount = imageCount;
        sci.imageFormat = format.format;
        sci.imageColorSpace = format.colorSpace;
        sci.imageExtent = extent;
        sci.imageArrayLayers = 1;
        sci.imageUsage = usage;
        sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
        sci.preTransform = caps.currentTransform;
        sci.compositeAlpha = (caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                                 ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                                 : VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
        sci.presentMode = mode;
        sci.clipped = VK_TRUE;
        sci.oldSwapchain = VK_NULL_HANDLE;
        VkSwapchainKHR swap = VK_NULL_HANDLE;
        if (vkCreateSwapchainKHR(device.get(), &sci, nullptr, &swap) != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkCreateSwapchainKHR failed\n");
            return;
        }
        swapchain.assign(device.get(), swap);

        uint32_t imgCount = 0;
        vkGetSwapchainImagesKHR(device.get(), swapchain.get(), &imgCount, nullptr);
        swapchainImages.resize(imgCount);
        vkGetSwapchainImagesKHR(device.get(), swapchain.get(), &imgCount, swapchainImages.data());
        swapchainValid_ = true;
        onscreen_ = true;
    }

    // present() onscreen 真实现；离屏/未绑 → no-op（权威 canvasImage 不受影响，离屏行为零变化）。
    // acquire → 显示源选择 → 布局过渡（UNDEFINED→TRANSFER_DST_OPTIMAL→PRESENT_SRC）→
    // vkCmdBlitImage → vkQueuePresentKHR，全程 GPU 内不读回。CPU 同步（mutex_ 串行、单帧在
    // 飞）：acquire 用现成 fence 等到 image 可用；blit 走 SubmitAndWait（fence-wait）；present
    // 无 wait semaphore 亦安全（同队列 + 已 fence-wait，无并发帧引用该 image）。
    void PresentLocked() {
        if (!onscreen_ || !swapchainValid_ || !canvasReady) {
            return;
        }
#ifdef DGCPAIN_ANDROID
        if (swapchainImages.empty()) {
            return;
        }
        vkResetFences(device.get(), 1, &fence.h);
        uint32_t imageIndex = 0;
        VkResult aq = vkAcquireNextImageKHR(device.get(), swapchain.get(), UINT64_MAX,
                                            VK_NULL_HANDLE, fence.get(), &imageIndex);
        // VK_SUCCESS 与 VK_SUBOPTIMAL_KHR 都返回了有效 image（SUBOPTIMAL 在 present 时再重建，
        // 避免每帧重建 churn）；仅 VK_ERROR_OUT_OF_DATE_KHR 需立即重建后重试一次本帧。
        if (aq == VK_ERROR_OUT_OF_DATE_KHR) {
            // swapchain 与 surface 尺寸失配/过时 → 重建后重试一次本帧。
            CreateSwapchainLocked();
            if (!swapchainValid_ || swapchainImages.empty()) {
                return;
            }
            vkResetFences(device.get(), 1, &fence.h);
            aq = vkAcquireNextImageKHR(device.get(), swapchain.get(), UINT64_MAX, VK_NULL_HANDLE,
                                       fence.get(), &imageIndex);
            if (aq != VK_SUCCESS && aq != VK_SUBOPTIMAL_KHR) {
                std::fprintf(stderr, "[VkBackend] vkAcquireNextImageKHR retry failed (%d)\n", aq);
                return;
            }
        } else if (aq == VK_ERROR_SURFACE_LOST_KHR) {
            std::fprintf(stderr, "[VkBackend] vkAcquireNextImageKHR surface lost\n");
            TeardownSwapchainLocked();  // 等下次 dgcSetSurface 重建
            return;
        } else if (aq != VK_SUCCESS && aq != VK_SUBOPTIMAL_KHR) {
            // 含 VK_TIMEOUT（暂无空闲 image，如 FIFO 满帧）：本帧跳过，不崩。
            std::fprintf(stderr, "[VkBackend] vkAcquireNextImageKHR failed (%d)\n", aq);
            return;
        }
        vkWaitForFences(device.get(), 1, &fence.h, VK_TRUE, UINT64_MAX);
        if (imageIndex >= swapchainImages.size()) {
            return;
        }

        BeginCommands();
        // 1) blit 源 = 显示源选择（同 readback 口径：tipHasContent_ 先 merge → displayImage）。
        VkImage srcImage = RecordDisplaySourceMergeLocked();
        // 源布局 GENERAL → TRANSFER_SRC_OPTIMAL（vkCmdBlitImage 要求 transfer src）。
        VkImageMemoryBarrier srcBar{};
        srcBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBar.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        srcBar.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcBar.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBar.image = srcImage;
        srcBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &srcBar);
        // 2) swapchain image UNDEFINED → TRANSFER_DST_OPTIMAL（每帧全量覆盖，无需保留旧内容）。
        VkImage dst = swapchainImages[imageIndex];
        VkImageMemoryBarrier dstBar{};
        dstBar.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstBar.srcAccessMask = 0;
        dstBar.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstBar.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        dstBar.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstBar.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBar.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstBar.image = dst;
        dstBar.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &dstBar);
        // 3) blit 全画布 → swapchain image（尺寸不一致时线性缩放）。
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {std::max(width, 1), std::max(height, 1), 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {(int32_t)swapchainExtent.width, (int32_t)swapchainExtent.height, 1};
        // 画布与 swapchain 通常同尺寸同格式（1:1 免缩放）；canvas 尺寸由离屏权威决定，可能与
        // swapchain 实际 extent 不同（缩放时才用得上 filter）。选 NEAREST：格式不一致时规格
        // 只允许 NEAREST，且 1:1 blit 时 filter 无影响（不引入 LINEAR 的格式采样特性依赖）。
        vkCmdBlitImage(commandBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_NEAREST);
        // 4) 收尾过渡：源回 GENERAL（后续 composite/readback 期待 GENERAL）；swapchain →
        // PRESENT_SRC（present engine 读）。
        VkImageMemoryBarrier srcBack{};
        srcBack.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        srcBack.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        srcBack.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        srcBack.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        srcBack.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        srcBack.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBack.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        srcBack.image = srcImage;
        srcBack.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier dstPresent{};
        dstPresent.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        dstPresent.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstPresent.dstAccessMask = 0;  // present engine read（隐式依赖由 vkQueuePresentKHR 承担）
        dstPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        dstPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        dstPresent.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstPresent.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        dstPresent.image = dst;
        dstPresent.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageMemoryBarrier postBars[2] = {srcBack, dstPresent};
        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 2,
                             postBars);
        SubmitAndWait();

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain.h;
        pi.pImageIndices = &imageIndex;
        VkResult pr = vkQueuePresentKHR(queue, &pi);
        if (pr == VK_ERROR_OUT_OF_DATE_KHR || pr == VK_SUBOPTIMAL_KHR) {
            CreateSwapchainLocked();  // 本帧已交付；下一帧用新 swapchain
        } else if (pr == VK_ERROR_SURFACE_LOST_KHR) {
            std::fprintf(stderr, "[VkBackend] vkQueuePresentKHR surface lost\n");
            TeardownSwapchainLocked();
        } else if (pr != VK_SUCCESS) {
            std::fprintf(stderr, "[VkBackend] vkQueuePresentKHR failed (%d)\n", pr);
        }
#endif  // DGCPAIN_ANDROID
    }

    void DestroyDevice() {
#ifdef DGCPAIN_RENDERDOC_ENABLED
        // 若从未触发过首次 composite（未画任何笔迹），确保未闭合的初始化抓帧在此收尾，避免悬挂。
        if (rdc && initCaptureOpen_) {
            rdc->endFrameCapture(RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(instance.get()));
            initCaptureOpen_ = false;
        }
#endif
        if (device == VK_NULL_HANDLE) {
            return;
        }
        vkDeviceWaitIdle(device);
        DestroyCanvas();
        // A8-5：onscreen swapchain → surface 先于 device/instance 释放（swapchain→surface→
        // device→instance）。surface 只需 instance、swapchain 需 device（此刻仍活）。
        swapchain.reset();
        surface.reset();
        swapchainImages.clear();
        swapchainExtent = {};
        swapchainValid_ = false;
        onscreen_ = false;
#ifdef DGCPAIN_ANDROID
        androidWindow_ = nullptr;
#endif
        fence.reset();
        pipeline.reset();
        pipelineLayout.reset();
        descriptorLayout.reset();
        mergePipeline.reset();
        mergePipelineLayout.reset();
        mergeDescriptorLayout.reset();
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
    // A8-5：非空 surface（ANativeWindow*）→ onscreen。仅 Android 建 VkSurfaceKHR +
    // VkSwapchainKHR；host 无可建 surface 的平台路径（headless/lavapipe），如实记录，
    // 离屏权威 canvas 不受影响。离屏 canvasImage 仍照常建（present 的 blit 源）。
#ifdef DGCPAIN_ANDROID
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->EnsureDevice();
    if (!impl_->deviceReady) {
        return;
    }
    // 画布仅当缺失/尺寸变化才重建（换窗/重复 setSurface 不清空既有笔迹）。
    if (impl_->width != w || impl_->height != h || !impl_->canvasReady) {
        impl_->CreateCanvas(w, h);
    }
    if (!impl_->SetupAndroidSurfaceLocked(reinterpret_cast<ANativeWindow*>(surface))) {
        return;
    }
    impl_->CreateSwapchainLocked();
#else
    std::fprintf(stderr, "[VkBackend] windowed present (A8-5) requires Android build\n");
#endif
}

void VkBackend::resize(int w, int h) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!impl_->deviceReady) {
        return;
    }
    impl_->CreateCanvas(w, h);
#ifdef DGCPAIN_ANDROID
    // A8-5：onscreen 态 surface 尺寸变化 → 重建 swapchain（离屏态无 swapchain，行为不变）。
    if (impl_->onscreen_) {
        impl_->CreateSwapchainLocked();
    }
#endif
}

void VkBackend::beginFrame() {
    // 离屏无需每帧 begin/end。
}

void VkBackend::composite(const std::vector<StampData>& stamps, bool predicted) {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->CompositeLocked(stamps, predicted);
}

void VkBackend::clearTip() {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->ClearTipLocked();
}

void VkBackend::clearCanvas(float r, float g, float b, float a) {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->ClearCanvasLocked(r, g, b, a);
}

void VkBackend::present() {
    // A8-5：onscreen 绑定下走 PresentLocked（acquire → 源选择 → blit → queuePresent，
    // 全程 GPU 内不读回）；离屏/未绑时 PresentLocked 内部早退，保持 no-op（§4.0.5 语义）。
    // 离屏每帧开销仅一次无竞争 mutex 加解锁（与 composite 同线程串行），可忽略。
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->PresentLocked();
}

void VkBackend::requestSnapshotRefresh() {
    // Bug #3（快照刷新节流）：非阻塞置位——只 store 一个原子标志，不碰 GPU、不等渲染线程。
    // 标志由 CompositeLocked 末尾 exchange(false) 消费（详见该处注释）；若消费时已置位才
    // 实际执行全画布 GPU→CPU 快照拷贝。连续绘制中 overCap 自动合批的 composite 不再付
    // 拷贝（Mali 弱 GPU 饱和 → 60→30 掉帧的根因）。
    impl_->snapshotRefreshRequested_.store(true, std::memory_order_release);
}

void VkBackend::flushReadbackCache() {
    // Bug #3（快照刷新节流）drain 收尾：同步执行一次全画布快照拷贝（加 mutex_ 串行化 GPU
    // 提交，与 composite/clear 同锁）。仅由阻塞的 dgcFlush（Engine::flush）在排空后调用——
    // 此刻渲染线程已排空（composited_==submitted_），mutex_ 空闲，无锁竞争；该路径本就
    // 阻塞、非每帧，追加一次 GPU 等待与既有 drain 语义一致。保证「最后一次 composite 之后
    // 的输入尾部」也被捕获进快照缓存（requestFlush 的原子标志只会被 drain 首个 post-request
    // composite 消费，无法覆盖尾部多批）。
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->RefreshReadbackCacheLocked();
}

void VkBackend::shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    impl_->DestroyDevice();
}

void VkBackend::initOffscreen(int w, int h) {
    std::lock_guard<std::mutex> lock(mutex_);
    // A8-5：切回离屏（dgcSetSurface(NULL) / 首启离屏）时释放已绑 surface/swapchain，
    // present 回 no-op。幂等：未绑过 onscreen 时 no-op。
    impl_->TeardownSwapchainLocked();
    impl_->EnsureDevice();
    impl_->CreateCanvas(w, h);
}

void VkBackend::readback(void* rgbaOut) {
    // bugfix（20fps 回退）：不再取 mutex_、不再触发 GPU 拷贝——直接从渲染线程维护的
    // 快照缓存 memcpy。彻底与渲染线程的 composite 提交解耦，不会等渲染线程；缓存只在
    // 完整 composite/clear 批提交完成后才刷新，因此永远是"完整画布"，不会读到半个 dab
    // （见 RefreshReadbackCacheLocked 与 docs/plans/bugfix-readback-blocks-render-thread.md）。
    if (rgbaOut == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(impl_->cache_mutex_);
    if (!impl_->cache_.empty()) {
        std::memcpy(rgbaOut, impl_->cache_.data(), impl_->cache_.size());
    }
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

#ifdef DGCPAIN_TEST_HOOKS
// Bug3 回归 hook（仅测试编译）：读 CompositeLocked 的 dispatch/barrier 计数。
std::uint64_t VkBackend::testDispatchCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->dispatchCount_;
}

std::uint64_t VkBackend::testBarrierCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->barrierCount_;
}

// Bug #3 回归 hook（仅测试编译）：读快照刷新 / composite 批提交计数。
std::uint64_t VkBackend::testSnapshotRefreshCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->snapshotRefreshCount_;
}

std::uint64_t VkBackend::testCompositeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->compositeCount_;
}

std::uint64_t VkBackend::testSubmitAndWaitCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return impl_->submitAndWaitCount_;
}
#endif
