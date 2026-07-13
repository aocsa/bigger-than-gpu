# Contributing to QUEST (Bigger Than GPU)

## Getting Started

```bash
git clone https://github.com/your-org/bigger-than-gpu.git
cd bigger-than-gpu
```

### Requirements

- C++23 compatible compiler (GCC 13+, Clang 17+)
- CMake 3.25+
- Google Test
- Snappy compression library

On macOS:

```bash
brew install cmake googletest snappy
```

## Development Loop

```bash
# 1. Configure and build (defaults to Debug, uses Ninja if available)
cmake -B build
cmake --build build

# 2. Run all unit tests
ctest --test-dir build --output-on-failure

# 3. Run a specific test
./build/tests/test_graph
```

> **Note:** The project requires C++23 features (`std::expected`, concepts, ranges, `std::format`).
> Make sure your compiler supports them.

### CMake Build Options

| Option | Default | Description |
|--------|---------|-------------|
| `CMAKE_BUILD_TYPE` | `Debug` | Build type. Auto-set to Debug for single-config generators (Makefiles, Ninja) |
| `QUEST_BUILD_TESTS` | `ON` | Build test binaries |
| `QUEST_BUILD_EXAMPLES` | `ON` | Build example binaries |
| `QUEST_SANITIZERS` | `""` | Enable sanitizers (semicolon-separated list) |

### Sanitizers

Use `QUEST_SANITIZERS` to catch memory bugs and data races at runtime:

```bash
# AddressSanitizer + UndefinedBehaviorSanitizer (catch memory errors)
cmake -B build -DQUEST_SANITIZERS="address;undefined"
cmake --build build && ctest --test-dir build

# ThreadSanitizer (catch data races — cannot combine with ASan)
cmake -B build-tsan -DQUEST_SANITIZERS=thread
cmake --build build-tsan && ctest --test-dir build-tsan
```

### Homebrew (macOS)

The root `CMakeLists.txt` adds `/opt/homebrew` to `CMAKE_PREFIX_PATH` on Apple platforms. You don't need to set it manually.

## Before Opening a PR

Run the same checks locally:

```bash
cmake --build build && ctest --test-dir build --output-on-failure
```

If you touched CMake configuration or added new source files, do a clean build:

```bash
rm -rf build && cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
```

## Where to Put Tests

| Type | Location | When to use |
|------|----------|-------------|
| Cache system (BatchHolder, DataBlock, BlockingQueue) | `tests/test_batch_holder.cpp`, `tests/test_cache.cpp`, `tests/test_cache_data.cpp`, `tests/test_blocking_queue.cpp` | Changes to data transfer, queues, or spilling |
| Taskflow (Graph, Operators, Ports) | `tests/test_graph.cpp` | New operators, DSL changes, graph execution |
| Memory management | `tests/test_memory.cpp`, `tests/test_memory_resource.cpp` | CPU/Disk tier changes, saturation logic |
| Data sources and Parquet | `tests/test_datasource.cpp`, `tests/test_parquet_reader.cpp`, `tests/test_column_vector.cpp`, `tests/test_schema.cpp` | File reading, column vectors, schema changes |
| Query engine (logical/physical plans) | `tests/test_logical_plan.cpp`, `tests/test_physical_plan.cpp`, `tests/test_query_builder.cpp` | Planner, optimizer, or query builder changes |
| End-to-end query execution | `tests/test_query_integration.cpp` | Full pipeline from scan to result |
| Serialization | `tests/test_record_batch_serializer.cpp` | RecordBatch serialization/deserialization |

Tests use [Google Test](https://github.com/google/googletest). See existing tests in `tests/` for examples.

### Running a Single Test

```bash
# Run one test binary
./build/tests/test_graph

# Run a specific test case by name
./build/tests/test_graph --gtest_filter="GraphTest.SimpleChain"

# Run tests matching a pattern
./build/tests/test_graph --gtest_filter="*Join*"

# List all available tests in a binary
./build/tests/test_graph --gtest_list_tests
```

### Adding a New Test

1. Create `tests/test_your_feature.cpp`
2. Register it in `tests/CMakeLists.txt`:

```cmake
add_executable(test_your_feature test_your_feature.cpp)
target_link_libraries(test_your_feature PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_your_feature)
```

## Code Organization

| What you're changing | Where to look |
|----------------------|---------------|
| Query builder DSL | `src/quest/builder/query_builder.hpp` |
| Logical plan (Scan, Selection, Projection) | `src/quest/logical_plan/` |
| Physical plan (execution operators) | `src/quest/physical_plan/` |
| Taskflow graph and DSL (`>>`, `+=`) | `src/quest/taskflow/graph.hpp`, `kpair.hpp` |
| Operators (base class, types) | `src/quest/taskflow/operator.hpp`, `operator_type.hpp` |
| Ports and connections | `src/quest/taskflow/port.hpp` |
| Cache system (BatchHolder, queues) | `src/quest/cache/` |
| Data blocks (CPU/Disk) | `src/quest/cache/data_block.hpp` |
| Parquet reader and data sources | `src/quest/datasource/` |
| Memory management (CPU/Disk tiers) | `src/quest/memory/memory_resource.hpp` |
| Build system | `CMakeLists.txt`, `tests/CMakeLists.txt` |
| Examples | `examples/` |

## Debugging Guide

If a query isn't behaving as expected, trace the execution in this order:

1. **`src/quest/builder/query_builder.hpp`** -- entry point, fluent API builds the logical plan
2. **`src/quest/logical_plan/`** -- Scan, Selection, Projection nodes
3. **`src/quest/physical_plan/`** -- logical-to-physical plan translation
4. **`src/quest/taskflow/graph.hpp`** -- execution graph construction and BFS traversal
5. **`src/quest/taskflow/operator.hpp`** -- operator `run()` loop
6. **`src/quest/cache/batch_holder.hpp`** -- data transfer between operators
7. **`src/quest/memory/memory_resource.hpp`** -- memory tier decisions and spilling

### Key Architectural Layers

```
QueryBuilder (fluent DSL)
    |
    v
Logical Plan (Scan -> Selection -> Projection)
    |
    v
Physical Plan (executable operators)
    |
    v
TaskFlow Graph (parallel execution with dependency scheduling)
    |
    v
Cache Layer (BatchHolder + BlockingQueue between operators)
    |
    v
Memory Management (CPU RAM <-> Disk spilling)
```

## Running Examples

```bash
./build/examples/simple_graph
./build/examples/join_example
./build/examples/parquet_reader_demo
./build/examples/logical_plan_to_physical_plan_demo
```

## Contribution Expectations

- Keep changes focused and small enough to review.
- Add or update tests with behavior changes.
- Run tests locally before opening a PR.
- If you change planner, operator, or cache architecture, update [ARCHITECTURE.md](ARCHITECTURE.md).

## Architecture Reference

See [ARCHITECTURE.md](ARCHITECTURE.md) for detailed documentation on:

- **TaskFlow System** -- Execution graph, operator scheduling, and the DSL
- **Cache System** -- BatchHolder types (Simple vs Concatenating), BlockingQueue
- **Memory Management** -- Two-tier hierarchy (CPU/Disk), automatic spilling
- **Execution Model** -- Parallel operator execution with flow control
- **Extension Points** -- How to create custom operators, data blocks, and memory tiers
