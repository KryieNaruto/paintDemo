# kernels/brush：自研 C++ 笔刷内核（B3-1，路线E §3a）。
# brush_kernel_factory.cpp 在顶层 add_library 源列表（镜像 render_backend_factory，始终编译），
# 本目录仅编译 Brush / BrushKernel 实现，并上抛 DGCPAIN_HAVE_BRUSH=1 给工厂条件编译。
target_sources(dgc_paint PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/brush.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/brush_kernel.cpp
)
target_compile_definitions(dgc_paint PRIVATE DGCPAIN_HAVE_BRUSH=1)
