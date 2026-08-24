add_executable(compile_null compile_null.cpp)
target_include_directories(compile_null PRIVATE ${CMAKE_SOURCE_DIR}) # 让 #include "core/..." 生效（复用既有冒烟源）
target_link_libraries(compile_null PRIVATE dgc_paint)
add_test(NAME compile_null COMMAND compile_null)
