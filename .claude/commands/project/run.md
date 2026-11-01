Build the project and run a specific example.

Available examples:
- `simple_graph`
- `parquet_reader_demo`
- `bloom_join_demo`

If an argument was provided, build and run that example:
```bash
cmake --build build && ./build/examples/$ARGUMENTS
```

If no argument was provided, list the available examples and ask which one to run.

Argument: $ARGUMENTS
