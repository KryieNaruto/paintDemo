# MSVC runtime 守卫回归（方案 A，无头可执行）。
# 模拟「MSVC + Debug + 无显式 runtime 覆盖」= LNK2038 bug 条件；
# include 真实修复模块 cmake/msvc_runtime.cmake（经 -DGCPAIN_FIX_MODULE 传入）。
set(MSVC ON)
if(NOT DEFINED GCPAIN_FIX_MODULE)
    message(FATAL_ERROR "GCPAIN_FIX_MODULE not set")
endif()
include("${GCPAIN_FIX_MODULE}")
if(NOT CMAKE_MSVC_RUNTIME_LIBRARY STREQUAL "MultiThreadedDLL")
    message(FATAL_ERROR
        "FAIL: MSVC runtime guard did not force MultiThreadedDLL (got '${CMAKE_MSVC_RUNTIME_LIBRARY}')")
endif()
message(STATUS "PASS: MSVC runtime guard forces MultiThreadedDLL")
