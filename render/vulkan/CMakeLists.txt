# render/vulkan：真实 Vulkan 离屏后端（B2-1）。
# 依赖 Vulkan 头/loader 与 shaderc 头/lib。shaderc 无 CMake config，走 find_path/find_library。

set(DGCPAIN_DEPS_ROOT "" CACHE PATH
    "Custom prefix for Vulkan/shaderc (e.g. extracted debs); empty = system default")

find_path(VULKAN_INCLUDE_DIR NAMES vulkan/vulkan.h
    HINTS ${DGCPAIN_DEPS_ROOT}/include)
find_library(VULKAN_LIBRARY NAMES vulkan
    HINTS ${DGCPAIN_DEPS_ROOT}/lib ${DGCPAIN_DEPS_ROOT}/lib/x86_64-linux-gnu)

find_path(SHADERC_INCLUDE_DIR NAMES shaderc/shaderc.hpp
    HINTS ${DGCPAIN_DEPS_ROOT}/include)
find_library(SHADERC_LIBRARY NAMES shaderc
    HINTS ${DGCPAIN_DEPS_ROOT}/lib ${DGCPAIN_DEPS_ROOT}/lib/x86_64-linux-gnu)

if(NOT VULKAN_INCLUDE_DIR OR NOT VULKAN_LIBRARY)
    message(FATAL_ERROR
        "Vulkan not found: install libvulkan-dev or set DGCPAIN_DEPS_ROOT")
endif()
if(NOT SHADERC_INCLUDE_DIR OR NOT SHADERC_LIBRARY)
    message(FATAL_ERROR
        "shaderc not found: install libshaderc-dev or set DGCPAIN_DEPS_ROOT")
endif()

# GLSL → 内嵌头（单一权威源是 brush_composite.comp；运行时 shaderc 编译，构建期不调 glslc）。
file(READ ${CMAKE_CURRENT_SOURCE_DIR}/brush_composite.comp GLSL_SOURCE)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/brush_composite_glsl.h.in
               ${CMAKE_CURRENT_BINARY_DIR}/brush_composite_glsl.h @ONLY)

target_sources(dgc_paint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/vk_backend.cpp)
target_include_directories(dgc_paint PRIVATE
    ${VULKAN_INCLUDE_DIR}
    ${SHADERC_INCLUDE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(dgc_paint PRIVATE ${VULKAN_LIBRARY} ${SHADERC_LIBRARY})
target_compile_definitions(dgc_paint PRIVATE DGCPAIN_HAVE_VULKAN=1)

# 供 tests/ 设 BUILD_RPATH，让共享 libshaderc 在运行期被找到（自定义前缀场景）。
get_filename_component(SHADERC_LIBRARY_DIR "${SHADERC_LIBRARY}" DIRECTORY)
set(SHADERC_LIBRARY_DIR "${SHADERC_LIBRARY_DIR}" PARENT_SCOPE)
