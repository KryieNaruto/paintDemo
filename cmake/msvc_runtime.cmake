# render/vulkan 的 shaderc_combined.lib（LunarG SDK）是 MSVC Release(/MD) 预编译，
# 而 Debug 默认 /MDd → LNK2038。MSVC 下显式钉 Release /MD 运行时与之一致。
# 仅当用户未显式设置 CMAKE_MSVC_RUNTIME_LIBRARY 时生效（尊重调用方显式覆盖）。
# 本模块同时被顶层 CMakeLists 与 tests/test_msvc_runtime_guard.cmake include。
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL" CACHE STRING "" FORCE)
endif()
