add_executable(simple_graph simple_graph.cpp)
target_link_libraries(simple_graph PRIVATE quest::quest)

add_executable(join_example join_example.cpp)
target_link_libraries(join_example PRIVATE quest::quest)

add_executable(parquet_reader_demo parquet_reader_demo.cpp)
target_link_libraries(parquet_reader_demo PRIVATE quest::quest)
