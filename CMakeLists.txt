cmake_minimum_required(VERSION 3.22)
project(dgc_paint LANGUAGES C CXX)

# MSVC 下钉 Release /MD 运行时，匹配 LunarG shaderc_combined.lib（见 cmake/msvc_runtime.cmake）。
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/msvc_runtime.cmake)

# ── 双 toolchain 分发 ──
if(CMAKE_SYSTEM_NAME STREQUAL "Android")
    set(DGCPAIN_ANDROID ON)
else()
    set(DGCPAIN_ANDROID OFF)
endif()

# ── 路线选项（绘画内核 L5）──
option(DGCPAIN_KERNEL_BRUSH   "Route E: self-developed C++ kernel" ON)   # 主线
option(DGCPAIN_KERNEL_MYPAINT "Route A: link libmypaint"            OFF)
option(DGCPAIN_KERNEL_GPU     "Route B: GPU compute kernel"         OFF)

# ── 路线选项（渲染后端 L4）──
option(DGCPAIN_RENDER_VULKAN  "Routes A/E/B: Vulkan compute"        ON)
option(DGCPAIN_RENDER_SKIA    "Route C: Skia GLES3"                 OFF)
option(DGCPAIN_RENDER_BGFX    "Route D: bgfx"                       OFF)

# 通用选项
option(DGCPAIN_BUILD_TESTS    "Build host unit tests"               ON)
option(DGCPAIN_BUILD_CLI      "Build CLI host (host only)"          ON)

# B5-4：RenderDoc 程序化抓帧（运行时注入，默认关，需 env DGC_RENDERDOC 才生效）。
option(DGCPAIN_RENDERDOC      "RenderDoc runtime injection (programmatic capture)" ON)

# Bug3（dab 合成孔洞）回归：把 VkBackend::CompositeLocked 的 dispatch/barrier 计数 hook
# 编进库，供 tests/test_composite_barrier_repro 断言「每次 dispatch 后都插了一次 barrier」。
# 仅测试构建开启（默认 OFF）；生产/发布构建保持 0 开销、不暴露测试专用符号。
option(DGCPAIN_TEST_HOOKS     "Compile VkBackend composite dispatch/barrier counters (test only)" OFF)

# 所有权/内存安全验证：ASan + LSan（host GCC/Clang 调试构建；Android 下不适用，勿加）。
option(DGCPAIN_SANITIZE "Enable AddressSanitizer + LeakSanitizer (host debug)" OFF)
if(DGCPAIN_SANITIZE)
    # Android NDK 的编译器 ID 也是 Clang，但 -fsanitize=leak 在 Android 上不可用，故显式排除。
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT DGCPAIN_ANDROID)
        add_compile_options(-fsanitize=address,leak -fno-omit-frame-pointer)
        add_link_options(-fsanitize=address,leak)
    else()
        message(WARNING "DGCPAIN_SANITIZE only supported on host GCC/Clang (skipped)")
    endif()
endif()

# ── 合法性检查（抄自 路线整理.md「顶层 CMake 选项设计」，与 §2.3 口径一致）──
if(DGCPAIN_KERNEL_GPU AND NOT DGCPAIN_RENDER_VULKAN)
    message(FATAL_ERROR "Route B (GPU kernel) requires Vulkan renderer")
endif()
if(DGCPAIN_KERNEL_MYPAINT AND DGCPAIN_KERNEL_BRUSH)
    message(FATAL_ERROR "Cannot enable both mypaint and brush kernel simultaneously")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── SDK 库目标：E0-3 先立住库与内部 sources；真实内核/渲染后续任务加入 ──
# B5-1：host 默认静态 libdgc_paint.a；Android 切 SHARED 产 libdgc_paint.so（§7.7）。
# 单一源清单 + DGCPAIN_BUILD_SHARED 开关，避免 if/else 复制源清单分叉。
option(DGCPAIN_BUILD_SHARED "Build dgc_paint as a shared library" ${DGCPAIN_ANDROID})
set(DGCPAIN_CORE_SOURCES
    core/null/null_paint_kernel.cpp
    core/null/null_render_backend.cpp
    core/engine.cpp
    core/stroke_predictor.cpp
    sdk_api/dgc_paint_c_api.cpp
    render/render_backend_factory.cpp
    kernels/brush/brush_kernel_factory.cpp
)
if(DGCPAIN_BUILD_SHARED)
    add_library(dgc_paint SHARED ${DGCPAIN_CORE_SOURCES})
    # B5-1 符号可见性收紧：只导出标记 DGC_API 的 C ABI 符号，隐藏内部 C++ 符号。
    set_target_properties(dgc_paint PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        C_VISIBILITY_PRESET  hidden
        VISIBILITY_INLINES_HIDDEN ON
    )
    # DGCPAIN_SHARED 经 PUBLIC 传给本目标与消费者：Windows 上据此切 dllexport/dllimport；
    # DGC_PAINT_BUILDING 标记本目标为库构建者（Windows 上 dllexport）。
    target_compile_definitions(dgc_paint
        PUBLIC DGCPAIN_SHARED=1
        PRIVATE DGC_PAINT_BUILDING)
else()
    add_library(dgc_paint STATIC ${DGCPAIN_CORE_SOURCES})
    # 静态库：不定义 DGCPAIN_SHARED，Windows 下 DGC_API 展开为空（无 dllimport/dllexport）。
endif()
target_include_directories(dgc_paint
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/sdk_api   # 唯一对外 include（B1-4 才放头）
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}           # 让 #include "core/..." 生效
)
if(DGCPAIN_ANDROID)
    # 诊断专用（DGCPAIN_PERF）：__android_log_print 需要 NDK liblog；真机 user 固件
    # 不转发 native stderr 进 logcat，fprintf 无输出，改走 Android 原生日志 API。
    target_link_libraries(dgc_paint PRIVATE log)
endif()
if(DGCPAIN_TEST_HOOKS)
    target_compile_definitions(dgc_paint PRIVATE DGCPAIN_TEST_HOOKS=1)
endif()

# ── 路线分发：E0-3 各子目录尚不存在，`if(EXISTS ...)` 守卫生效为空操作；
#    Null 桩已在库目标内无条件编译，作为当前默认 L4/L5。B2-1/B3-1 落地后：
#    各自新增 kernels/brush/、render/vulkan/ 等 CMakeLists.txt，此处守卫自动接管 ──
if(DGCPAIN_KERNEL_BRUSH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/kernels/brush/CMakeLists.txt)
    add_subdirectory(kernels/brush)
endif()
if(DGCPAIN_KERNEL_MYPAINT AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/kernels/mypaint/CMakeLists.txt)
    add_subdirectory(kernels/mypaint)
endif()
if(DGCPAIN_KERNEL_GPU AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/kernels/gpu/CMakeLists.txt)
    add_subdirectory(kernels/gpu)
endif()
if(DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/render/vulkan/CMakeLists.txt)
    add_subdirectory(render/vulkan)
endif()
if(DGCPAIN_RENDERDOC AND DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/render/renderdoc/CMakeLists.txt)
    add_subdirectory(render/renderdoc)
endif()
if(DGCPAIN_RENDER_SKIA AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/render/skia/CMakeLists.txt)
    add_subdirectory(render/skia)
endif()
if(DGCPAIN_RENDER_BGFX AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/render/bgfx/CMakeLists.txt)
    add_subdirectory(render/bgfx)
endif()

# ── CLI 宿主（host only，B5-2）：在 tests 之前，供 test_cli 以 TARGET dgc_cli 守卫引用 ──
if(DGCPAIN_BUILD_CLI AND NOT DGCPAIN_ANDROID)
    add_subdirectory(cli)
endif()

# ── 测试（host only）──
if(DGCPAIN_BUILD_TESTS AND NOT DGCPAIN_ANDROID)
    enable_testing()
    add_subdirectory(tests)
endif()
