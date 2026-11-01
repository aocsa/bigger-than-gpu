# Google Test configuration (Homebrew prefix set in root CMakeLists.txt)
find_package(GTest REQUIRED)

enable_testing()
include(GoogleTest)

# Test executables
add_executable(test_blocking_queue test_blocking_queue.cpp)
target_link_libraries(test_blocking_queue PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_blocking_queue DISCOVERY_TIMEOUT 60)

add_executable(test_graph test_graph.cpp)
target_link_libraries(test_graph PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_graph DISCOVERY_TIMEOUT 60)

add_executable(test_datasource test_datasource.cpp)
target_link_libraries(test_datasource PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_datasource DISCOVERY_TIMEOUT 60)

add_executable(test_cache test_cache.cpp)
target_link_libraries(test_cache PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_cache DISCOVERY_TIMEOUT 60)

add_executable(test_memory test_memory.cpp)
target_link_libraries(test_memory PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_memory DISCOVERY_TIMEOUT 60)

add_executable(test_parquet_reader test_parquet_reader.cpp)
target_link_libraries(test_parquet_reader PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_parquet_reader DISCOVERY_TIMEOUT 60)

add_executable(test_column_vector test_column_vector.cpp)
target_link_libraries(test_column_vector PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_column_vector DISCOVERY_TIMEOUT 60)

add_executable(test_schema test_schema.cpp)
target_link_libraries(test_schema PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_schema DISCOVERY_TIMEOUT 60)

add_executable(test_batch_holder test_batch_holder.cpp)
target_link_libraries(test_batch_holder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_batch_holder DISCOVERY_TIMEOUT 60)

add_executable(test_cache_data test_cache_data.cpp)
target_link_libraries(test_cache_data PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_cache_data DISCOVERY_TIMEOUT 60)

add_executable(test_memory_resource test_memory_resource.cpp)
target_link_libraries(test_memory_resource PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_memory_resource DISCOVERY_TIMEOUT 60)

add_executable(test_expr_builder test_expr_builder.cpp)
target_link_libraries(test_expr_builder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_expr_builder DISCOVERY_TIMEOUT 60)

add_executable(test_query_builder test_query_builder.cpp)
target_link_libraries(test_query_builder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_query_builder DISCOVERY_TIMEOUT 60)

add_executable(test_logical_plan test_logical_plan.cpp)
target_link_libraries(test_logical_plan PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_logical_plan DISCOVERY_TIMEOUT 60)

add_executable(test_physical_expr test_physical_expr.cpp)
target_link_libraries(test_physical_expr PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_physical_expr DISCOVERY_TIMEOUT 60)

add_executable(test_query_integration test_query_integration.cpp)
target_link_libraries(test_query_integration PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_query_integration DISCOVERY_TIMEOUT 60)

add_executable(test_result result_test.cpp)
target_link_libraries(test_result PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_result DISCOVERY_TIMEOUT 60)

add_executable(test_record_batch_serializer test_record_batch_serializer.cpp)
target_link_libraries(test_record_batch_serializer PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_record_batch_serializer DISCOVERY_TIMEOUT 60)

add_executable(test_barrier_config test_barrier_config.cpp)
target_link_libraries(test_barrier_config PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_barrier_config DISCOVERY_TIMEOUT 60)

add_executable(test_operator_roles test_operator_roles.cpp)
target_link_libraries(test_operator_roles PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_operator_roles DISCOVERY_TIMEOUT 60)

add_executable(test_meta_pipeline test_meta_pipeline.cpp)
target_link_libraries(test_meta_pipeline PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_meta_pipeline DISCOVERY_TIMEOUT 60)

add_executable(test_pipeline_executor test_pipeline_executor.cpp)
target_link_libraries(test_pipeline_executor PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_pipeline_executor DISCOVERY_TIMEOUT 60)

add_executable(test_pipeline_spill test_pipeline_spill.cpp)
target_link_libraries(test_pipeline_spill PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_pipeline_spill DISCOVERY_TIMEOUT 60)

add_executable(test_graph_bridge test_graph_bridge.cpp)
target_link_libraries(test_graph_bridge PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_graph_bridge DISCOVERY_TIMEOUT 60)

add_executable(test_limit test_limit.cpp)
target_link_libraries(test_limit PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_limit DISCOVERY_TIMEOUT 60)

add_executable(test_union test_union.cpp)
target_link_libraries(test_union PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_union DISCOVERY_TIMEOUT 60)

add_executable(test_bindable_scan test_bindable_scan.cpp)
target_link_libraries(test_bindable_scan PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_bindable_scan DISCOVERY_TIMEOUT 60)

add_executable(test_generate_print test_generate_print.cpp)
target_link_libraries(test_generate_print PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_generate_print DISCOVERY_TIMEOUT 60)

add_executable(test_manual_query_walkthrough test_manual_query_walkthrough.cpp)
target_link_libraries(test_manual_query_walkthrough PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_manual_query_walkthrough DISCOVERY_TIMEOUT 60)

add_executable(test_prepared_query test_prepared_query.cpp)
target_link_libraries(test_prepared_query PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_prepared_query DISCOVERY_TIMEOUT 60)

add_executable(test_graph_builder test_graph_builder.cpp)
target_link_libraries(test_graph_builder PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_graph_builder DISCOVERY_TIMEOUT 60)

add_executable(test_runtime_tuning test_runtime_tuning.cpp)
target_link_libraries(test_runtime_tuning PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_runtime_tuning DISCOVERY_TIMEOUT 60)

add_executable(test_blazing_parity test_blazing_parity.cpp)
target_link_libraries(test_blazing_parity PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_blazing_parity DISCOVERY_TIMEOUT 60)

add_executable(test_bloom_filter test_bloom_filter.cpp)
target_link_libraries(test_bloom_filter PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_bloom_filter DISCOVERY_TIMEOUT 60)

add_executable(test_hash_aggregate test_hash_aggregate.cpp)
target_link_libraries(test_hash_aggregate PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_hash_aggregate DISCOVERY_TIMEOUT 60)

add_executable(test_sort test_sort.cpp)
target_link_libraries(test_sort PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_sort DISCOVERY_TIMEOUT 60)

add_executable(test_hash_join test_hash_join.cpp)
target_link_libraries(test_hash_join PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_hash_join DISCOVERY_TIMEOUT 60)
if(TARGET bloom_join_demo)
    add_test(NAME BloomJoin.Demo COMMAND bloom_join_demo)
endif()

get_property(quest_local_targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
foreach(target IN LISTS quest_local_targets)
    target_compile_options(${target} PRIVATE ${QUEST_WARNING_FLAGS})
endforeach()
