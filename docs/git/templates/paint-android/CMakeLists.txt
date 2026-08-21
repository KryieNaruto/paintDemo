# paint-android consumer — skeleton only
cmake_minimum_required(VERSION 3.22)
project(paint_android LANGUAGES C CXX)

add_subdirectory(sdk)

# 只允许 #include "dgc_paint_c_api.h"；禁止 include sdk/core/

# 示例：JNI 模块链接 SDK（实现放在本消费者仓库，不在 sdk/ 内）
# add_library(paint_jni SHARED jni/paint_jni.cpp)
# target_link_libraries(paint_jni PRIVATE dgc_paint android log)
