# MSVC runtime 守卫回归（方案 A，无头可执行）：模拟 MSVC Debug 下强制 MultiThreadedDLL，
# 直接 include 真实修复模块 cmake/msvc_runtime.cmake（经 -DGCPAIN_FIX_MODULE 传入）。
add_test(NAME test_msvc_runtime_debug_guard
         COMMAND ${CMAKE_COMMAND}
                 -DGCPAIN_FIX_MODULE=${CMAKE_CURRENT_SOURCE_DIR}/../cmake/msvc_runtime.cmake
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/test_msvc_runtime_guard.cmake)

# 运行期找到共享 libshaderc.so.1（自定义前缀场景，如提取的 .deb）。
# SHADERC_LIBRARY_DIR 由 render/vulkan/CMakeLists.txt 上抛到父作用域。
if(DEFINED SHADERC_LIBRARY_DIR AND SHADERC_LIBRARY_DIR)
    list(APPEND CMAKE_BUILD_RPATH "${SHADERC_LIBRARY_DIR}")
endif()

add_executable(compile_null compile_null.cpp)
target_include_directories(compile_null PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..) # 让 #include "core/..." 生效（复用既有冒烟源）
target_link_libraries(compile_null PRIVATE dgc_paint)
add_test(NAME compile_null COMMAND compile_null)

add_executable(test_ring_buffer test_ring_buffer.cpp)
target_include_directories(test_ring_buffer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_ring_buffer PRIVATE dgc_paint)
add_test(NAME test_ring_buffer COMMAND test_ring_buffer)

add_executable(test_engine test_engine.cpp)
target_include_directories(test_engine PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
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
if(DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../render/vulkan/CMakeLists.txt)
    add_executable(test_offscreen test_offscreen.cpp)
    target_include_directories(test_offscreen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(test_offscreen PRIVATE dgc_paint)
    add_test(NAME test_offscreen COMMAND test_offscreen)

    # B4-1：dab 形状光栅化 GPU 化对照（CPU 参考 vs GPU compute 像素 diff）。
    add_executable(test_gpu_dab_raster test_gpu_dab_raster.cpp)
    target_include_directories(test_gpu_dab_raster PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(test_gpu_dab_raster PRIVATE dgc_paint)
    add_test(NAME test_gpu_dab_raster COMMAND test_gpu_dab_raster)

    # Bug3（dab 合成孔洞）回归：真实 5-dab 数据（RenderDoc 抓帧 a-capture_16.rdc 解出，
    # 见 test_composite_barrier_repro.cpp 内常量数组）跑 VkBackend::composite()，断言
    # (a) 覆盖区内 alpha>0（barrier 不破坏覆盖正确性）(b) dispatchCount == barrierCount
    # （每次 dispatch 后都插了一次同步 barrier）。
    # 依赖 DGCPAIN_TEST_HOOKS 编进 dgc_paint 库（顶层 option 打开时才有 hook 符号，否则
    # 链接报 undefined reference），故随 option 一起注册——默认 OFF 时本测试不注册，
    # 与 sdk/CMakeLists.txt 的 option 对齐（这是唯一语义正确的接法）。
    if(DGCPAIN_TEST_HOOKS)
        add_executable(test_composite_barrier_repro test_composite_barrier_repro.cpp)
        target_include_directories(test_composite_barrier_repro PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_composite_barrier_repro PRIVATE dgc_paint)
        target_compile_definitions(test_composite_barrier_repro PRIVATE DGCPAIN_TEST_HOOKS=1)
        add_test(NAME test_composite_barrier_repro COMMAND test_composite_barrier_repro)

        # Bug #3（快照刷新节流）回归：依赖 DGCPAIN_TEST_HOOKS 编进库的 VkBackend 快照刷新/
        # composite 批计数 hook（dgcTestSnapshotRefreshCount/dgcTestCompositeCount），
        # 随 option 一起注册（默认 OFF 不注册，与顶层 option 对齐）。
        add_executable(test_snapshot_refresh_throttle test_snapshot_refresh_throttle.cpp)
        target_include_directories(test_snapshot_refresh_throttle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_snapshot_refresh_throttle PRIVATE dgc_paint)
        target_compile_definitions(test_snapshot_refresh_throttle PRIVATE DGCPAIN_TEST_HOOKS=1)
        add_test(NAME test_snapshot_refresh_throttle COMMAND test_snapshot_refresh_throttle)
    endif()

    # B5-4：RenderDoc 程序化抓帧 —— 降级等价 + 所有权循环（无 renderdoc 库环境）。
    if(DGCPAIN_RENDERDOC)
        add_executable(test_renderdoc_capture test_renderdoc_capture.cpp)
        target_include_directories(test_renderdoc_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_renderdoc_capture PRIVATE dgc_paint)
        add_test(NAME test_renderdoc_capture COMMAND test_renderdoc_capture)
    endif()
endif()

# B3-1：自研 C++ 笔刷内核主链（host 无条件，无 Vulkan 依赖）。
if(DGCPAIN_KERNEL_BRUSH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../kernels/brush/CMakeLists.txt)
    add_executable(test_brush_kernel test_brush_kernel.cpp)
    target_include_directories(test_brush_kernel PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(test_brush_kernel PRIVATE dgc_paint)
    add_test(NAME test_brush_kernel COMMAND test_brush_kernel)

    # B3-1：离屏集成（C API 端到端 + 颜色烘焙），需 Vulkan。
    if(DGCPAIN_RENDER_VULKAN AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/../render/vulkan/CMakeLists.txt)
        add_executable(test_brush_offscreen test_brush_offscreen.cpp)
        target_include_directories(test_brush_offscreen PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_brush_offscreen PRIVATE dgc_paint)
        add_test(NAME test_brush_offscreen COMMAND test_brush_offscreen)

        # bugfix 复现：readback 不先 drain 时画布完整性（模拟 paint-pc 每帧读回）。
        add_executable(test_readback_drain test_readback_drain.cpp)
        target_include_directories(test_readback_drain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_readback_drain PRIVATE dgc_paint)
        add_test(NAME test_readback_drain COMMAND test_readback_drain)

        # bugfix 回归补充：模拟 paint-pc 真实 GUI 循环——笔画进行中逐帧读回（无 flush），
        # 与 test_readback_drain（仅笔画结束后读回一次）互补，覆盖"边画边读"场景。
        add_executable(test_midstroke_readback test_midstroke_readback.cpp)
        target_include_directories(test_midstroke_readback PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_midstroke_readback PRIVATE dgc_paint)
        add_test(NAME test_midstroke_readback COMMAND test_midstroke_readback)

        # bugfix 回归：dgcReadbackPixels 不应阻塞在渲染线程 composite 上（PC 20fps 回退）。
        add_executable(test_perf_regression test_perf_regression.cpp)
        target_include_directories(test_perf_regression PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
        target_link_libraries(test_perf_regression PRIVATE dgc_paint)
        add_test(NAME test_perf_regression COMMAND test_perf_regression)

        # P7-1：真正连续不间断输入下的孔洞修复 + GUI 侧读回无阻塞尖峰回归测试。
        add_executable(test_continuous_input_regression test_continuous_input_regression.cpp)
        target_include_directories(test_continuous_input_regression PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_continuous_input_regression PRIVATE dgc_paint)
        add_test(NAME test_continuous_input_regression COMMAND test_continuous_input_regression)

        # bugfix：笔刷内核基础参数 0-2 必须真实作用于渲染（红：设 RADIUS=40 无效果）。
        add_executable(test_brush_setting_applies test_brush_setting_applies.cpp)
        target_include_directories(test_brush_setting_applies PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_brush_setting_applies PRIVATE dgc_paint)
        add_test(NAME test_brush_setting_applies COMMAND test_brush_setting_applies)

        # bugfix：modeler 激活后必须渲染出可用笔画（红：未调 fixedtime 时塌缩成点）。
        add_executable(test_modeler_stroke_renders test_modeler_stroke_renders.cpp)
        target_include_directories(test_modeler_stroke_renders PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_modeler_stroke_renders PRIVATE dgc_paint)
        add_test(NAME test_modeler_stroke_renders COMMAND test_modeler_stroke_renders)

        # bugfix：modeler 参数必须可观测地改变输出（红：全零时间戳下两参数输出相同）。
        add_executable(test_modeler_param_changes_output test_modeler_param_changes_output.cpp)
        target_include_directories(test_modeler_param_changes_output PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_modeler_param_changes_output PRIVATE dgc_paint)
        add_test(NAME test_modeler_param_changes_output COMMAND test_modeler_param_changes_output)

        # bugfix：modeler 路径确定性——同输入同参数两次逐字节一致。
        add_executable(test_modeler_deterministic test_modeler_deterministic.cpp)
        target_include_directories(test_modeler_deterministic PRIVATE ${CMAKE_SOURCE_DIR})
        target_link_libraries(test_modeler_deterministic PRIVATE dgc_paint)
        add_test(NAME test_modeler_deterministic COMMAND test_modeler_deterministic)
    endif()
endif()

# B1-8 所有权压力测试：多轮 create/destroy 无泄漏、无崩溃（ASan/LSan 兜底）。
add_executable(test_ownership_loop test_ownership_loop.c)
target_link_libraries(test_ownership_loop PRIVATE dgc_paint)
add_test(NAME test_ownership_loop COMMAND test_ownership_loop)

# B1-7 确定性机制单测：同 seed 同序列 / 不同 seed 异 / Replay 回放 / 固定步进 / C API 冒烟。
# B5-3 扩展：端到端「同 seed 像素级一致」+ golden PNG 逐字节 diff（定位 fixtures/）。
add_executable(test_determinism test_determinism.cpp)
target_include_directories(test_determinism PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_determinism PRIVATE dgc_paint)
target_compile_definitions(test_determinism PRIVATE
    DGC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
add_test(NAME test_determinism COMMAND test_determinism)

# B1-5 白盒移植 Ink Stroke Modeler → core/stroke_predictor：平滑 / 预测 + is_predicted /
# 覆盖 / 确定性 / 泄漏循环。
add_executable(test_stroke_predictor test_stroke_predictor.cpp)
target_include_directories(test_stroke_predictor PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(test_stroke_predictor PRIVATE dgc_paint)
add_test(NAME test_stroke_predictor COMMAND test_stroke_predictor)

# B5-2：CLI 集成测试（shell 调 dgc_cli 二进制，不编译 script_runner.cpp 进本目标）。
# 需 dgc_cli 已建（DGCPAIN_BUILD_CLI=ON 且顶层 add_subdirectory(cli) 先于 tests）。
if(DGCPAIN_BUILD_CLI AND TARGET dgc_cli)
    add_executable(test_cli test_cli.cpp)
    target_include_directories(test_cli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/..)
    target_link_libraries(test_cli PRIVATE dgc_paint)
    target_compile_definitions(test_cli PRIVATE
        DGC_CLI_PATH="$<TARGET_FILE:dgc_cli>"
        DGC_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
    add_dependencies(test_cli dgc_cli)
    add_test(NAME test_cli COMMAND test_cli)
endif()
