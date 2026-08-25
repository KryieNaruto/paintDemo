# cli/dgc_cli：SDK C API 的第一个 host-only 宿主（B5-2，§4.0.6）。
# 只 include 公开头 sdk_api/，只链 dgc_paint；不 include core/render/kernels 内部头。
# DGCPAIN_BUILD_CLI 仅 host 开，Android preset 不构建（顶层 CMakeLists 已 NOT DGCPAIN_ANDROID 守卫）。

# 运行期找到共享 libshaderc.so.1（自定义前缀场景，如提取的 .deb）。
# SHADERC_LIBRARY_DIR 由 render/vulkan/CMakeLists.txt 上抛到顶层作用域。
if(DEFINED SHADERC_LIBRARY_DIR AND SHADERC_LIBRARY_DIR)
    list(APPEND CMAKE_BUILD_RPATH "${SHADERC_LIBRARY_DIR}")
endif()

add_executable(dgc_cli
    main.cpp
    script_runner.cpp
)

# 仅指向 sdk_api/（公开头）与 third_party/（nlohmann/json.hpp）；不加 ${CMAKE_SOURCE_DIR}，
# 从机制上杜绝 #include "core/..." 等内部头误用（Pimpl 边界）。
target_include_directories(dgc_cli PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk_api
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(dgc_cli PRIVATE dgc_paint)
