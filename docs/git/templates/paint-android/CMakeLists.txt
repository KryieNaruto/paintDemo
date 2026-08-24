cmake_minimum_required(VERSION 3.22)
project(paint_android LANGUAGES C CXX)

# 本模板只演示如何把 paintDemo SDK 编进消费者的 JNI 库。
# JNI 胶水 / Compose / Ink 由消费者自备；窗口句柄经 dgcSetSurface 传入 SDK。

# sdk/ 是 git submodule（路径固定 sdk/），指向 paintDemo 仓库。
add_subdirectory(sdk)

# 消费者的 JNI 库（native 胶水，消费者自备实现）。
add_library(paint_android_jni SHARED
    jni/paint_android_jni.cpp
)

# 只链接 dgc_paint。不要链接 SDK 内部 target。
target_link_libraries(paint_android_jni PRIVATE dgc_paint)
