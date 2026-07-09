# Google Test configuration (Homebrew prefix set in root CMakeLists.txt)
find_package(GTest REQUIRED)

enable_testing()
include(GoogleTest)

# Test executables
add_executable(test_blocking_queue test_blocking_queue.cpp)
target_link_libraries(test_blocking_queue PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_blocking_queue)

add_executable(test_graph test_graph.cpp)
target_link_libraries(test_graph PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_graph)

add_executable(test_datasource test_datasource.cpp)
target_link_libraries(test_datasource PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_datasource)

add_executable(test_cache test_cache.cpp)
target_link_libraries(test_cache PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_cache)

add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_memory)

add_executable(test_parquet_reader test_parquet_reader.cpp)
target_link_libraries(test_parquet_reader PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_parquet_reader)

add_executable(test_column_vector test_column_vector.cpp)
target_link_libraries(test_column_vector PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_column_vector)

add_executable(test_schema test_schema.cpp)
target_link_libraries(test_schema PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_schema)

add_executable(test_batch_holder test_batch_holder.cpp)
target_link_libraries(test_batch_holder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_batch_holder)

add_executable(test_cache_data test_cache_data.cpp)
target_link_libraries(test_cache_data PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_cache_data)

add_executable(test_memory_resource test_memory_resource.cpp)
target_link_libraries(test_memory_resource PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_memory_resource)

add_executable(test_expr_builder test_expr_builder.cpp)
target_link_libraries(test_expr_builder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_expr_builder)

add_executable(test_generator test_generator.cpp)
target_link_libraries(test_generator PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_generator)

add_executable(test_query_builder test_query_builder.cpp)
target_link_libraries(test_query_builder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_query_builder)

add_executable(test_streaming_query_builder test_streaming_query_builder.cpp)
target_link_libraries(test_streaming_query_builder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_streaming_query_builder)

add_executable(test_logical_plan test_logical_plan.cpp)
target_link_libraries(test_logical_plan PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_logical_plan)

add_executable(test_physical_plan test_physical_plan.cpp)
target_link_libraries(test_physical_plan PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_physical_plan)

add_executable(test_query_integration test_query_integration.cpp)
target_link_libraries(test_query_integration PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_query_integration)

add_executable(test_result result_test.cpp)
target_link_libraries(test_result PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_result)

add_executable(test_record_batch_serializer test_record_batch_serializer.cpp)
target_link_libraries(test_record_batch_serializer PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_record_batch_serializer)

add_executable(test_barrier_config test_barrier_config.cpp)
target_link_libraries(test_barrier_config PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_barrier_config)

add_executable(test_operator_roles test_operator_roles.cpp)
target_link_libraries(test_operator_roles PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_operator_roles)

add_executable(test_meta_pipeline test_meta_pipeline.cpp)
target_link_libraries(test_meta_pipeline PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_meta_pipeline)

add_executable(test_pipeline_executor test_pipeline_executor.cpp)
target_link_libraries(test_pipeline_executor PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_pipeline_executor)
