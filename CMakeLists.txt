cmake_minimum_required(VERSION 3.22)
project(dgc_paint LANGUAGES C CXX)

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
add_library(dgc_paint
    core/null/null_paint_kernel.cpp
    core/null/null_render_backend.cpp
    core/engine.cpp
    sdk_api/dgc_paint_c_api.cpp
    render/render_backend_factory.cpp
)
target_include_directories(dgc_paint
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/sdk_api   # 唯一对外 include（B1-4 才放头）
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}           # 让 #include "core/..." 生效
)

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
