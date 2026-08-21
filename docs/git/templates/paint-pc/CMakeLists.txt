# paint-pc consumer — skeleton only
cmake_minimum_required(VERSION 3.22)
project(paint_pc LANGUAGES C CXX)

add_subdirectory(sdk)

# 示例：桌面可执行文件链接 SDK + 本仓库的 GLFW/ImGui
# add_executable(paint_pc src/main.cpp)
# target_link_libraries(paint_pc PRIVATE dgc_paint glfw imgui)
