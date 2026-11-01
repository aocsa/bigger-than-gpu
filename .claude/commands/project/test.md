Build and run a specific test by name or pattern.

If the argument matches a test binary name (e.g., "graph", "cache", "memory"), run that binary:
```bash
cmake --build build && ./build/tests/test_$ARGUMENTS
```

If the argument contains a `*` or looks like a GTest filter pattern (e.g., "*Join*", "GraphTest.SimpleChain"), find the right test binary and run with `--gtest_filter`:
```bash
cmake --build build && ./build/tests/test_<binary> --gtest_filter="$ARGUMENTS"
```

If no argument was provided, list available test binaries:
```bash
ls build/tests/test_*
```

Common test binaries: graph, graph_builder, cache, memory, datasource, parquet_reader, query_builder, logical_plan, physical_expr, query_integration, pipeline_executor, meta_pipeline, operator_roles, limit, union, batch_holder, blocking_queue, schema, column_vector, expr_builder, record_batch_serializer, result.

Argument: $ARGUMENTS
