add_executable(simple_graph simple_graph.cpp)
target_link_libraries(simple_graph PRIVATE quest::quest)

add_executable(parquet_reader_demo parquet_reader_demo.cpp)
target_link_libraries(parquet_reader_demo PRIVATE quest::quest)

add_executable(bloom_join_demo bloom_join_demo.cpp)
target_link_libraries(bloom_join_demo PRIVATE quest::quest)

get_property(quest_local_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS quest_local_targets)
    target_compile_options(${target} PRIVATE ${QUEST_WARNING_FLAGS})
endforeach()
