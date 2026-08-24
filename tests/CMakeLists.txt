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

# 纯 C 冒烟：B1-6 新增 8 个 v3.0 函数的返回码语义。
add_executable(test_c_api_b1_6 test_c_api_b1_6.c)
target_link_libraries(test_c_api_b1_6 PRIVATE dgc_paint)
add_test(NAME test_c_api_b1_6 COMMAND test_c_api_b1_6)
