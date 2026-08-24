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
