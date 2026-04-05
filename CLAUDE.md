# QUEST — Bigger Than GPU

C++23 batch processing framework with two-tier memory (CPU/Disk) and declarative dataflow execution.
Inspired by BlazingSQL's "Bigger than GPU" architecture, refocused for CPU -> Disk tiers.

## Build & Test

```bash
# Dependencies (macOS): brew install cmake googletest snappy
mkdir -p build && cd build && cmake .. && make
ctest --output-on-failure                          # all tests
./tests/test_graph --gtest_filter="*Join*"         # specific tests
```

## Architecture Layers

```
QueryBuilder          src/quest/builder/query_builder.hpp    (fluent DSL entry point)
    |
Logical Plan          src/quest/logical_plan/                (Scan, Selection, Projection)
    |
Physical Plan         src/quest/physical_plan/               (executable operators)
    |
TaskFlow Graph        src/quest/taskflow/                    (parallel execution, BFS scheduling)
    |
Cache Layer           src/quest/cache/                       (BatchHolder + BlockingQueue)
    |
Memory Management     src/quest/memory/memory_resource.hpp   (CPU RAM <-> Disk spilling)
```

## Key Directories

| Area | Path |
|------|------|
| Query builder DSL | `src/quest/builder/query_builder.hpp` |
| Logical plan nodes & expressions | `src/quest/logical_plan/` |
| Physical plan & expressions | `src/quest/physical_plan/` |
| Graph, operators, ports, DSL (`>>`, `+=`) | `src/quest/taskflow/` |
| Cache (BatchHolder, DataBlock, BlockingQueue) | `src/quest/cache/` |
| Parquet reader, data sources, column vectors | `src/quest/datasource/` |
| Memory tiers (CPU/Disk) | `src/quest/memory/` |
| Tests (Google Test) | `tests/` |
| Examples | `examples/` |
| Detailed architecture & contributing docs | `docs/dev/` |

## Ownership Conventions

- `unique_ptr` for exclusive ownership: expression trees (`LogicalExprPtr`), plan nodes (`LogicalPlanPtr`, `PhysicalPlanPtr`), compression codecs, column vectors.
- `shared_ptr` for shared resources: `IBatchHolder` (shared between source/target operators), `MemoryResourceManager` (one per graph), `DataSource` (shared across plan layers), `ColumnVector` in `RecordBatch` (zero-copy slicing), `DataBlock` (multi-consumer cache).
- `weak_ptr` for back-references: operators reference their owning `Graph`.
- Zero raw `new`/`delete`. Use `make_unique`/`make_shared` exclusively.

## Adding a New Operator

1. Subclass `Operator` in `src/quest/taskflow/operator.hpp`
2. Set `OperatorType` enum in `operator_type.hpp`
3. Implement `run()` and `can_you_throttle_my_input()`
4. Wire it into the graph with the DSL: `graph += scan >> my_op >> output;`
5. Add tests in `tests/`

## Adding a New Test

1. Create `tests/test_feature.cpp`
2. Register in `tests/CMakeLists.txt`:
```cmake
add_executable(test_feature test_feature.cpp)
target_link_libraries(test_feature PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_feature)
```

## Query Engine Usage

```cpp
#include "quest/builder/query_builder.hpp"
using namespace quest;

auto result = QueryBuilder::FromParquet("data.parquet");
auto batches = std::move(result.value())
    .Where(Col("age") > Lit(21))
    .Select({"id", "name"})
    .ExecuteCollect();
```

- `QueryBuilder` methods are rvalue-qualified (`&&`) — chain with `std::move`
- `ExecuteCollect()` gathers all results; `Execute(callback)` streams batch-by-batch
- `BuildLogicalPlan()` / `BuildPhysicalPlan()` for plan inspection without execution

## C++ Standards Used

`std::expected` for error handling (Result/Status types), concepts for type constraints,
ranges for iteration, `std::format` for formatting. Requires GCC 13+ or Clang 17+.

## Library Target

`quest::quest` — link via `target_link_libraries(your_target PRIVATE quest::quest)`.
Public includes rooted at `src/`, main header: `#include "quest/quest.hpp"`.

## Data Flow Pattern

Operators produce/consume `DataBlock`s through `BatchHolder`s (thread-safe `BlockingQueue`).
Two `BatchHolder` types: **Simple** (FIFO pull) and **Concatenating** (merge all into one).
Memory manager auto-spills from CPU to disk at 85% saturation. Disk cleanup on destruction.

## Debugging Trace Order

query_builder.hpp -> logical_plan/ -> physical_plan/ -> taskflow/graph.hpp ->
taskflow/operator.hpp -> cache/batch_holder.hpp -> memory/memory_resource.hpp
