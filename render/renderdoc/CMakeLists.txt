# render/renderdoc：RenderDoc 程序化抓帧（B5-4，内部模块，不进 C ABI）。
# 纯运行时注入（POSIX dlopen / Windows LoadLibrary），无链接依赖；
# 头为 vendor 单头 third_party/renderdoc/renderdoc_app.h（纯 C，无符号链接需求）。
target_sources(dgc_paint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderdoc_capture.cpp)
target_include_directories(dgc_paint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party/renderdoc)
# 编译期标记，供 vk_backend.cpp 用 #ifdef 决定是否挂接 Start/EndCapture（DGCPAIN_RENDERDOC=OFF 时
# 完全不编入 RenderDoc 代码，vk_backend.cpp 零引用，避免链接缺符号）。
target_compile_definitions(dgc_paint PRIVATE DGCPAIN_RENDERDOC_ENABLED=1)
