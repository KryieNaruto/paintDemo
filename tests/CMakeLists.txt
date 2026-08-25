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

# B5-1 纯 C headless C API 闭环（host 无条件）：无窗口走完整 C API 会话，
# readback 像素断言 + exportPNG；Null 后端分支跳过像素断言仍通过。
add_executable(test_c_api_headless test_c_api_headless.c)
target_link_libraries(test_c_api_headless PRIVATE dgc_paint)
add_test(NAME test_c_api_headless COMMAND test_c_api_headless)

# B2-1：真实 Vulkan 离屏合成验证（host lavapipe headless）。
if(DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_SOURCE_DIR}/render/vulkan/CMakeLists.txt)
    add_executable(test_offscreen test_offscreen.cpp)
    target_include_directories(test_offscreen PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_offscreen PRIVATE dgc_paint)
    add_test(NAME test_offscreen COMMAND test_offscreen)
endif()

# B3-1：自研 C++ 笔刷内核主链（host 无条件，无 Vulkan 依赖）。
if(DGCPAIN_KERNEL_BRUSH AND EXISTS ${CMAKE_SOURCE_DIR}/kernels/brush/CMakeLists.txt)
    add_executable(test_brush_kernel test_brush_kernel.cpp)
    target_include_directories(test_brush_kernel PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_brush_kernel PRIVATE dgc_paint)
    add_test(NAME test_brush_kernel COMMAND test_brush_kernel)

    # B3-1：离屏集成（C API 端到端 + 颜色烘焙），需 Vulkan。
    if(DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_SOURCE_DIR}/render/vulkan/CMakeLists.txt)
        add_executable(test_brush_offscreen test_brush_offscreen.cpp)
        target_include_directories(test_brush_offscreen PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_brush_offscreen PRIVATE dgc_paint)
        add_test(NAME test_brush_offscreen COMMAND test_brush_offscreen)
    endif()
endif()

# B1-8 所有权压力测试：多轮 create/destroy 无泄漏、无崩溃（ASan/LSan 兜底）。
add_executable(test_ownership_loop test_ownership_loop.c)
target_link_libraries(test_ownership_loop PRIVATE dgc_paint)
add_test(NAME test_ownership_loop COMMAND test_ownership_loop)

# B1-7 确定性机制单测：同 seed 同序列 / 不同 seed 异 / Replay 回放 / 固定步进 / C API 冒烟。
# B5-3 扩展：端到端「同 seed 像素级一致」+ golden PNG 逐字节 diff（定位 fixtures/）。
add_executable(test_determinism test_determinism.cpp)
target_include_directories(test_determinism PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_determinism PRIVATE dgc_paint)
target_compile_definitions(test_determinism PRIVATE
    DGC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME test_determinism COMMAND test_determinism)

# B1-5 白盒移植 Ink Stroke Modeler → core/stroke_predictor：平滑 / 预测 + is_predicted /
# 覆盖 / 确定性 / 泄漏循环。
add_executable(test_stroke_predictor test_stroke_predictor.cpp)
target_include_directories(test_stroke_predictor PRIVATE ${CMAKE_SOURCE_DIR})
target_link_libraries(test_stroke_predictor PRIVATE dgc_paint)
add_test(NAME test_stroke_predictor COMMAND test_stroke_predictor)

# B5-2：CLI 集成测试（shell 调 dgc_cli 二进制，不编译 script_runner.cpp 进本目标）。
# 需 dgc_cli 已建（DGCPAIN_BUILD_CLI=ON 且顶层 add_subdirectory(cli) 先于 tests）。
if(DGCPAIN_BUILD_CLI AND TARGET dgc_cli)
    add_executable(test_cli test_cli.cpp)
    target_include_directories(test_cli PRIVATE ${CMAKE_SOURCE_DIR})
    target_link_libraries(test_cli PRIVATE dgc_paint)
    target_compile_definitions(test_cli PRIVATE
        DGC_CLI_PATH="$<TARGET_FILE:dgc_cli>"
        DGC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_dependencies(test_cli dgc_cli)
    add_test(NAME test_cli COMMAND test_cli)
endif()
