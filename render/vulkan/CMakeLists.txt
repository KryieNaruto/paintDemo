# render/vulkan：真实 Vulkan 离屏后端（B2-1）。
# 依赖 Vulkan 头/loader。shader 编译路径分两条：
#   - host：find_library(shaderc) + 运行时 shaderc 编译（B2-1 不变）。
#   - Android：NDK 无 libshaderc，改走 glslc 构建期预编译 SPIR-V + 内嵌（B5-1）。
# shaderc 无 CMake config，走 find_path/find_library。

set(DGCPAIN_DEPS_ROOT "" CACHE PATH
    "Custom prefix for Vulkan/shaderc (e.g. extracted debs); empty = system default")

if(WIN32)
    # ── Windows：LunarG Vulkan SDK 布局（%VULKAN_SDK%\Include / %VULKAN_SDK%\Lib）──
    # vulkan-1.lib / shaderc_combined.lib 为 Windows 命名（非 Linux 的 vulkan / shaderc）。
    find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h
        HINTS ${DGCPAIN_DEPS_ROOT}/include $ENV{VULKAN_SDK}/Include)
    find_library(VULKAN_LIBRARY NAMES vulkan-1
        HINTS ${DGCPAIN_DEPS_ROOT}/lib $ENV{VULKAN_SDK}/Lib)
else()
    find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h
        HINTS ${DGCPAIN_DEPS_ROOT}/include)
    find_library(VULKAN_LIBRARY NAMES vulkan
        HINTS ${DGCPAIN_DEPS_ROOT}/lib ${DGCPAIN_DEPS_ROOT}/lib/x86_64-linux-gnu)
endif()

if(NOT VULKAN_INCLUDE_DIR OR NOT VULKAN_LIBRARY)
    message(FATAL_ERROR
        "Vulkan not found: install libvulkan-dev / LunarG Vulkan SDK or set DGCPAIN_DEPS_ROOT")
endif()

# Vulkan 头/loader 为两条路径共用（NDK sysroot 自带 vulkan/vulkan.h 与 libvulkan.so，
# 经 CMAKE_FIND_ROOT_PATH 可直接命中）。
target_sources(dgc_paint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk_backend.cpp)
target_include_directories(dgc_paint PRIVATE
    ${VULKAN_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dgc_paint PRIVATE ${VULKAN_LIBRARY})
target_compile_definitions(dgc_paint PRIVATE DGCPAIN_HAVE_VULKAN=1)

if(DGCPAIN_ANDROID)
    # ── Android：NDK 无 libshaderc，改走 glslc 构建期预编译 SPIR-V + 内嵌字节数组 ──
    # 注：glslc 在 configure 期（execute_process）编译一次；brush_composite.comp 改动后
    # 需重跑 cmake（--preset android-arm64）重新生成。
    find_program(GLSLC_BIN NAMES glslc)
    if(NOT GLSLC_BIN)
        # NDK 自带 glslc（shader-tools/<host-tag>/glslc），通常不在宿主 PATH，显式探测。
        # Windows 下文件是 glslc.exe：不带 .exe 会让 EXISTS 判假 → glslc not found → configure 失败。
        set(_ndk_glslc "${CMAKE_ANDROID_NDK}/shader-tools/${ANDROID_HOST_TAG}/glslc")
        if(NOT EXISTS "${_ndk_glslc}")
            set(_ndk_glslc "${_ndk_glslc}.exe")
        endif()
        if(EXISTS "${_ndk_glslc}")
            set(GLSLC_BIN "${_ndk_glslc}")
        endif()
    endif()
    if(NOT GLSLC_BIN)
        message(FATAL_ERROR
            "glslc not found (NDK provides it under shader-tools/<host-tag>/glslc)")
    endif()

    execute_process(
        # -g：内嵌 OpSource debug 信息，RenderDoc 的 Pipeline State 可显示 GLSL 源码。
        COMMAND ${GLSLC_BIN} -g -fshader-stage=compute
            ${CMAKE_CURRENT_SOURCE_DIR}/brush_composite.comp
            -o ${CMAKE_CURRENT_BINARY_DIR}/brush_composite.spv
        RESULT_VARIABLE _glslc_rc
        OUTPUT_VARIABLE _glslc_out
        ERROR_VARIABLE _glslc_err)
    if(NOT _glslc_rc EQUAL 0)
        message(FATAL_ERROR "glslc failed compiling brush_composite.comp:\n${_glslc_err}")
    endif()

    # .spv 二进制 → C 字节数组（每 2 hex → 0x..,，末位逗号 C++20 合法）。
    file(READ ${CMAKE_CURRENT_BINARY_DIR}/brush_composite.spv _spv HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," SPV_C_ARRAY "${_spv}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/brush_composite_spv.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/brush_composite_spv.h @ONLY)

    # A8-2：merge.comp（canvas+tip→display）同款预编译 SPIR-V + 内嵌头。
    execute_process(
        COMMAND ${GLSLC_BIN} -g -fshader-stage=compute
            ${CMAKE_CURRENT_SOURCE_DIR}/merge.comp
            -o ${CMAKE_CURRENT_BINARY_DIR}/merge.spv
        RESULT_VARIABLE _merge_glslc_rc
        OUTPUT_VARIABLE _merge_glslc_out
        ERROR_VARIABLE _merge_glslc_err)
    if(NOT _merge_glslc_rc EQUAL 0)
        message(FATAL_ERROR "glslc failed compiling merge.comp:\n${_merge_glslc_err}")
    endif()
    file(READ ${CMAKE_CURRENT_BINARY_DIR}/merge.spv _merge_spv HEX)
    string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," MERGE_SPV_C_ARRAY "${_merge_spv}")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/merge_spv.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/merge_spv.h @ONLY)
    # 回归：merge_spv.h 必须内嵌 merge.spv 的字节，不是 brush_composite 的。模板
    # merge_spv.h.in 若误用 @SPV_C_ARRAY@（brush 变量）会灌进 brush 的 SPIR-V，导致
    # merge 管线在 Mali 上编译失败 → 预测激活时绑空管线崩溃（见 A8-2 真机 SIGSEGV）。
    file(READ ${CMAKE_CURRENT_BINARY_DIR}/merge_spv.h _merge_hdr)
    string(FIND "${_merge_hdr}" "${MERGE_SPV_C_ARRAY}" _merge_spv_pos)
    if(_merge_spv_pos EQUAL -1)
        message(FATAL_ERROR
            "merge_spv.h 内嵌 SPIR-V 与 merge.spv 不一致（merge_spv.h.in 变量串用回归）")
    endif()
    target_compile_definitions(dgc_paint PRIVATE DGCPAIN_PRECOMPILED_SPV=1)
else()
    # ── host：运行时 shaderc 编译（B2-1 不变）──
    if(WIN32)
        # Windows：LunarG SDK 提供 shaderc_combined.lib（非 Linux 的 shaderc）。
        find_path(SHADERC_INCLUDE_DIR NAMES shaderc/shaderc.hpp
            HINTS ${DGCPAIN_DEPS_ROOT}/include $ENV{VULKAN_SDK}/Include)
        find_library(SHADERC_LIBRARY NAMES shaderc_combined
            HINTS ${DGCPAIN_DEPS_ROOT}/lib $ENV{VULKAN_SDK}/Lib)
    else()
        find_path(SHADERC_INCLUDE_DIR NAMES shaderc/shaderc.hpp
            HINTS ${DGCPAIN_DEPS_ROOT}/include)
        find_library(SHADERC_LIBRARY NAMES shaderc
            HINTS ${DGCPAIN_DEPS_ROOT}/lib ${DGCPAIN_DEPS_ROOT}/lib/x86_64-linux-gnu)
    endif()

    if(NOT SHADERC_INCLUDE_DIR OR NOT SHADERC_LIBRARY)
        message(FATAL_ERROR
            "shaderc not found: install libshaderc-dev / LunarG Vulkan SDK or set DGCPAIN_DEPS_ROOT")
    endif()

    # GLSL → 内嵌头（单一权威源是 brush_composite.comp；运行时 shaderc 编译，构建期不调 glslc）。
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/brush_composite.comp GLSL_SOURCE)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/brush_composite_glsl.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/brush_composite_glsl.h @ONLY)

    # A8-2：merge.comp 内嵌头（运行时 shaderc 编译；复用 @GLSL_SOURCE@ 占位符）。
    file(READ ${CMAKE_CURRENT_SOURCE_DIR}/merge.comp GLSL_SOURCE)
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/merge_glsl.h.in
                   ${CMAKE_CURRENT_BINARY_DIR}/merge_glsl.h @ONLY)

    target_include_directories(dgc_paint PRIVATE ${SHADERC_INCLUDE_DIR})
    target_link_libraries(dgc_paint PRIVATE ${SHADERC_LIBRARY})

    # 供 tests/ 设 BUILD_RPATH，让共享 libshaderc 在运行期被找到（自定义前缀场景）。
    get_filename_component(SHADERC_LIBRARY_DIR "${SHADERC_LIBRARY}" DIRECTORY)
    set(SHADERC_LIBRARY_DIR "${SHADERC_LIBRARY_DIR}" PARENT_SCOPE)
endif()
