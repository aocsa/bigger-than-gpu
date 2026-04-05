# Query Builder Refactor: Sync + Streaming Split

## Context

The original `query_builder.hpp` was a 565-line monolithic header mixing four concerns:
expression DSL, C++20 coroutine generator, synchronous push-based execution, and
streaming pull-based execution. This refactor splits it into focused, single-responsibility
files while preserving backward compatibility.

### Problem

1. **Single Responsibility violation** — one file owned expression building, coroutine machinery, and two fundamentally different execution models
2. **Unnecessary compilation cost** — sync-only consumers paid for `<coroutine>` includes
3. **Unclear API boundary** — users had to discover which `Execute*` method matched their use case from a single class with mixed concerns

### Solution

**Composition via `QueryPlan`** — a shared value type that owns the `LogicalPlanPtr` and
all plan-building logic. Two separate builders wrap it and add only their execution methods.

---

## Architecture

### File Organization

```
src/quest/builder/
├── expr_builder.hpp              # ExprBuilder, Col(), Lit(), LiteralValue concept
├── generator.hpp                 # Generator<T> coroutine, BatchGenerator alias
├── query_plan.hpp                # QueryPlan — shared fluent API + plan building
├── query_builder.hpp             # QueryBuilder — sync execution
└── streaming_query_builder.hpp   # StreamingQueryBuilder — coroutine execution
```

### Include DAG (no cycles)

```
                 <coroutine>
                     ↑
generator.hpp ───────┘
     ↑
     │               logical_expr.hpp
     │                     ↑
     │              expr_builder.hpp
     │                     ↑
     │              query_plan.hpp ←── logical_plan.hpp, physical_plan.hpp,
     │                  ↑    ↑         data_source.hpp, result.hpp
     │                  │    │
streaming_query_    query_builder.hpp
  builder.hpp        (sync only — NO <coroutine>)
```

### Class Relationships

```
┌─────────────────────┐
│     QueryPlan       │  Shared plan-building logic
│─────────────────────│
│ FromParquet()       │  Factory methods
│ From()              │
│ Where()             │  Fluent API (all &&-qualified)
│ Select() ×3        │
│ BuildLogicalPlan()  │  Plan extraction
│ BuildPhysicalPlan() │
│ ToString()          │  Inspection
│ schema()            │
│ operator bool()     │
└─────────┬───────────┘
          │ composed by
    ┌─────┴──────┐
    │            │
    ▼            ▼
┌──────────┐  ┌──────────────────────┐
│QueryBuilder│  │StreamingQueryBuilder │
│──────────│  │──────────────────────│
│Execute() │  │ExecuteStream()       │
│ExecWith  │  │  → BatchGenerator    │
│ Control()│  │                      │
│ExecCollect│  │                      │
└──────────┘  └──────────────────────┘
  (sync)        (coroutine/pull-based)
```

### Design Decisions

| Decision | Chosen | Alternatives Considered |
|----------|--------|------------------------|
| Shared logic | Composition (`QueryPlan`) | CRTP, raw duplication |
| Why not CRTP | `&&`-qualified fluent methods returning `Derived` add template complexity disproportionate to 5-6 methods | Works but harder to debug, worse error messages |
| Why not duplication | 3 `Select` overloads have 5-8 lines of loop+construction each — 40 lines of real logic to maintain in two places | Simpler, but maintenance risk |
| `QUEST_QUERY` macro | Removed (unused anywhere) | Keep for future use |

---

## What Changed

### New Files Created (4)

| File | Lines | Content |
|------|-------|---------|
| `src/quest/builder/expr_builder.hpp` | ~130 | `LiteralValue` concept, `ExprBuilder` class with operator overloads, `Col()`, `Lit()` |
| `src/quest/builder/generator.hpp` | ~170 | `Generator<T>` coroutine with `promise_type`, iterator, `has_next()`, `next()`, `BatchGenerator` alias |
| `src/quest/builder/query_plan.hpp` | ~175 | `QueryPlan` class, `BatchCallback`/`BatchCallbackWithControl` concepts, factory methods, fluent API |
| `src/quest/builder/streaming_query_builder.hpp` | ~165 | `StreamingQueryBuilder` wrapping `QueryPlan` with `ExecuteStream()` |

### Modified Files (2)

| File | Change |
|------|--------|
| `src/quest/builder/query_builder.hpp` | Rewritten from 565 → ~180 lines. Now contains only `QueryBuilder` (sync execution) wrapping `QueryPlan`. Re-exports `expr_builder.hpp` and `query_plan.hpp` for backward compatibility. |
| `tests/CMakeLists.txt` | Added 3 new test targets: `test_expr_builder`, `test_generator`, `test_streaming_query_builder` |

### Test Reorganization

| Test File | Content | Tests |
|-----------|---------|-------|
| `tests/test_expr_builder.cpp` | ExprBuilder operators, Rule of Five, concept static_asserts | 18 |
| `tests/test_generator.cpp` | `Generator<int>` coroutine mechanics | 4 |
| `tests/test_query_builder.cpp` | `QueryBuilder` sync execution (From, Select, Where, Execute, ExecuteCollect, ExecuteWithControl) | ~15 |
| `tests/test_streaming_query_builder.cpp` | `StreamingQueryBuilder` with `ExecuteStream` (range-for, `next()`) | 2 |

### Backward Compatibility

- `query_builder.hpp` includes `expr_builder.hpp` and `query_plan.hpp` — existing code that only does `#include "quest/builder/query_builder.hpp"` compiles unchanged
- `tests/test_query_integration.cpp` — no changes needed
- `examples/logical_plan_to_physical_plan_demo.cpp` — no changes needed

---

## How to Build and Test

### Prerequisites (macOS)

```bash
brew install cmake googletest snappy
```

### Build

```bash
# From repo root
cmake -S . -B build
cmake --build build
```

### Run All Tests

```bash
cd build && ctest --output-on-failure
```

Expected: **473 tests passed**, 1 skipped (missing Parquet test file), 0 failed.

### Run Specific Test Binaries

```bash
# Expression builder tests
./build/tests/test_expr_builder

# Generator coroutine tests
./build/tests/test_generator

# Sync query builder tests
./build/tests/test_query_builder

# Streaming query builder tests
./build/tests/test_streaming_query_builder

# Integration tests (end-to-end)
./build/tests/test_query_integration
```

### Run Specific Test Cases

```bash
# Filter by test name
./build/tests/test_expr_builder --gtest_filter="ExprBuilderTest.Equality"

# Run all Rule of Five tests
./build/tests/test_expr_builder --gtest_filter="ExprBuilderRuleOfFive.*"

# List all tests in a binary
./build/tests/test_query_builder --gtest_list_tests
```

### Run Example Binaries

```bash
./build/examples/simple_graph
./build/examples/join_example
./build/examples/parquet_reader_demo
./build/examples/logical_plan_to_physical_plan_demo
```

---

## Usage Examples

### Sync Query Builder (push-based)

```cpp
#include "quest/builder/query_builder.hpp"

using namespace quest;

// Callback execution
auto result = QueryBuilder::FromParquet("data.parquet");
if (!result) { /* handle error */ }

std::move(result.value())
    .Where(Col("salary") > Lit(50000))
    .Select({"id", "name", "salary"})
    .Execute([](datasource::RecordBatch batch) {
        // Process each batch
    });

// Collect all results
auto batches = QueryBuilder::FromParquet("data.parquet")
    .value()
    .Where(Col("active") == Lit(true))
    .ExecuteCollect();

// Early termination
std::move(builder)
    .ExecuteWithControl([&count](datasource::RecordBatch batch) {
        return ++count < 10;  // Stop after 10 batches
    });
```

### Streaming Query Builder (pull-based with backpressure)

```cpp
#include "quest/builder/streaming_query_builder.hpp"

using namespace quest;

auto result = StreamingQueryBuilder::FromParquet("large_file.parquet");
if (!result) { /* handle error */ }

// Range-for: consumer controls pace
for (auto batch_result : std::move(result.value())
         .Where(Col("salary") > Lit(50000))
         .Select({"id", "name"})
         .ExecuteStream()) {
    if (!batch_result) { break; }
    process(std::move(*batch_result));
}

// Manual iteration with next()
auto stream = std::move(builder).ExecuteStream();
while (auto batch_opt = stream.next()) {
    auto& batch_result = *batch_opt;
    if (batch_result.has_value()) {
        process(std::move(*batch_result));
    }
}
```

---

## Next Steps for Future Sessions

### Immediate (continuation of this work)

1. **True streaming execution** — `StreamingQueryBuilder::ExecuteStream()` currently collects all batches before yielding (see the `@note` in the implementation). The fix requires making `PhysicalPlan::Execute` coroutine-aware or running it on a separate producer thread that feeds batches into a channel consumed by the coroutine.

2. **Add `GroupBy` / `OrderBy` / `Limit` to both builders** — the fluent API in `QueryPlan` currently only has `Where()` and `Select()`. New plan nodes need to be added to both `logical_plan.hpp` and `physical_plan.hpp` first (see `docs/dev/future_improvement.md` Priority 4 for the full list).

3. **Consider a `QueryPlan::ToStreaming()` / `QueryPlan::ToSync()` conversion** — allow users to build a plan once and then choose execution mode at the terminal step, rather than committing to a builder type upfront.

### From future_improvement.md (prioritized)

| Priority | Issue | File(s) |
|----------|-------|---------|
| P1 | Thread safety in `Graph::Execute()` — `processed_edges` data race | `taskflow/graph.cpp` |
| P1 | Scheduler busy-wait spin (no CV, burns CPU) | `taskflow/graph.cpp` |
| P1 | `SimpleOperator::run()` uses `ErrorCode::Invalid` for end-of-stream | `taskflow/operator.cpp` |
| P1 | `MaterializedColumnVector::IsNull()` always returns false | `physical_expr.hpp` |
| P2 | Replace exceptions with `Result<>` in serializer, port, memory manager | Multiple |
| P3 | `ConcatenatingBatchHolder::ConcatenateAll()` multi-block not implemented | `batch_holder.cpp` |
| P4 | Missing plan nodes: Limit → Aggregate → Sort → Join | `logical_plan/`, `physical_plan/` |
| P5 | ZSTD/GZIP compression codecs for Parquet | `parquet/compression.hpp` |
| P6 | Vectorized expression evaluation (currently row-by-row) | `physical_expr.hpp` |

### Key Files Reference

| Area | Path |
|------|------|
| Query builder (sync) | `src/quest/builder/query_builder.hpp` |
| Query builder (streaming) | `src/quest/builder/streaming_query_builder.hpp` |
| Shared plan logic | `src/quest/builder/query_plan.hpp` |
| Expression DSL | `src/quest/builder/expr_builder.hpp` |
| Generator coroutine | `src/quest/builder/generator.hpp` |
| Logical plan nodes | `src/quest/logical_plan/logical_plan.hpp` |
| Physical plan nodes | `src/quest/physical_plan/physical_plan.hpp` |
| Physical expressions | `src/quest/physical_plan/expressions/physical_expr.hpp` |
| Taskflow graph/DSL | `src/quest/taskflow/graph.hpp`, `operator.hpp`, `port.hpp`, `kpair.hpp` |
| Cache layer | `src/quest/cache/batch_holder.hpp`, `blocking_queue.hpp`, `data_block.hpp` |
| Parquet reader | `src/quest/datasource/parquet/` |
| Memory management | `src/quest/memory/memory_resource.hpp` |
| Future improvements | `docs/dev/future_improvement.md` |
| Architecture reference | `docs/dev/ARQUITECTURE.md` |
| Contributing guide | `docs/dev/CONTRIBUTING.md` |
| Tests CMakeLists | `tests/CMakeLists.txt` |
