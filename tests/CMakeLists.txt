# 运行期找到共享 libshaderc.so.1（自定义前缀场景，如提取的 .deb）。
# SHADERC_LIBRARY_DIR 由 render/vulkan/CMakeLists.txt 上抛到父作用域。
if(DEFINED SHADERC_LIBRARY_DIR AND SHADERC_LIBRARY_DIR)
    list(APPEND CMAKE_BUILD_RPATH "${SHADERC_LIBRARY_DIR}")
endif()

add_executable(compile_null compile_null.cpp)
target_include_directories(compile_null PRIVATE ${CMAKE_SOURCE_DIR}) # 让 #include "core/..." 生效（复用既有冒烟源）
target_link_libraries(compile_null PRIVATE dgc_paint)
add_test(NAME compile_null COMMAND compile_null)

add_executable(test_ring_buffer test_ring_buffer.cpp)
target_include_directories(test_ring_buffer PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_ring_buffer PRIVATE dgc_paint)
add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

add_executable(test_engine test_engine.cpp)
target_include_directories(test_engine PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_engine PRIVATE dgc_paint)
add_test(NAME test_engine COMMAND test_engine)

# 纯 C 冒烟：验证对外头可被 C 编译器解析且实现已链接进 dgc_paint。
add_executable(test_c_api_header test_c_api_header.c)
target_link_libraries(test_c_api_header PRIVATE dgc_paint)
add_test(NAME test_c_api_header COMMAND test_c_api_header)

# 纯 C 冒烟：B1-6 新增 8 个 v3.0 函数的返回码语义（B2-1 接线离屏三函数）。
add_executable(test_c_api_b1_6 test_c_api_b1_6.c)
target_link_libraries(test_c_api_b1_6 PRIVATE dgc_paint)
add_test(NAME test_c_api_b1_6 COMMAND test_c_api_b1_6)

# B2-1：真实 Vulkan 离屏合成验证（host lavapipe headless）。
if(DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_SOURCE_DIR}/render/vulkan/CMakeLists.txt)
    add_executable(test_offscreen test_offscreen.cpp)
    target_include_directories(test_offscreen PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_offscreen PRIVATE dgc_paint)
    add_test(NAME test_offscreen COMMAND test_offscreen)
endif()

# B1-8 所有权压力测试：多轮 create/destroy 无泄漏、无崩溃（ASan/LSan 兜底）。
add_executable(test_ownership_loop test_ownership_loop.c)
target_link_libraries(test_ownership_loop PRIVATE dgc_paint)
add_test(NAME test_ownership_loop COMMAND test_ownership_loop)

# B1-7 确定性机制单测：同 seed 同序列 / 不同 seed 异 / Replay 回放 / 固定步进 / C API 冒烟。
add_executable(test_determinism test_determinism.cpp)
target_include_directories(test_determinism PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_determinism PRIVATE dgc_paint)
add_test(NAME test_determinism COMMAND test_determinism)
