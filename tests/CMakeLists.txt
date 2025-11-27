# Tests will use a simple test framework or catch2
# For now, create simple test executables

add_executable(test_blocking_queue test_blocking_queue.cpp)
target_link_libraries(test_blocking_queue PRIVATE quest::quest)
add_test(NAME blocking_queue COMMAND test_blocking_queue)

add_executable(test_graph test_graph.cpp)
target_link_libraries(test_graph PRIVATE quest::quest)
add_test(NAME graph COMMAND test_graph)
