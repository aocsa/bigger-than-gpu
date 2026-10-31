---
name: new-operator
description: Scaffolds a new QUEST operator with enum entry, class file, test, and CMake registration. Trigger: "add operator", "new operator", "create operator", "implement X operator"
---

# New Operator Scaffold

Creates a complete QUEST pipeline operator with all required files and registrations.
All operators run through the **push pipeline engine** (`Graph::Execute()` →
`MetaPipeline::Build` → `PipelineExecutor`). The old thread-per-operator `run()` loop
is deleted; operators declare one or more pipeline roles instead.

## Read These Files First

- `src/quest/taskflow/operator.hpp` — `Operator` base class + pipeline role virtuals
- `src/quest/taskflow/operator_type.hpp` — `OperatorType` enum and helper functions
- `src/quest/taskflow/operators/` — existing operators as reference implementations
- `tests/CMakeLists.txt` — test registration pattern
- `docs/dev/future_improvement.md` — known bugs and missing implementations

## Step 1: Gather Requirements (then STOP)

Ask the user:
1. **Operator name** (e.g., "Sort", "Aggregate")
2. **Role** — which pipeline role(s) does the operator play?
   - **Streaming** (Filter, Project, Limit): transforms one batch → one batch, fused intra-pipeline. Override `ProcessBatch`.
   - **Source** (TableScan, Generate): generates batches from an external source. Override `IsSource()` + `GetNextBatch()`.
   - **Sink/Blocking** (Aggregate, Sort, Join): accumulates state in `Sink()`, emits via `FinalizeSink()` + `GetNextBatch()`. Appears in two pipelines.
3. **Port count** — single input (default) or multi-input (named ports)?
4. **Brief description** of what it does

STOP and wait for answers before proceeding.

## Step 2: Add Enum Entry

File: `src/quest/taskflow/operator_type.hpp`

1. Add new entry to `OperatorType` enum in the appropriate section (Source/Transformation/Set/Aggregation/Sort/Join/Limit-Output)
2. Add `case` to `to_string()` function
3. If source operator: add to `is_source_operator()`
4. If blocking/terminal operator: add to `is_blocking_operator()` / `is_terminal_operator()`
5. If multi-input: add to `is_multi_input_operator()`

## Step 3: Create Operator Class

File: `src/quest/taskflow/operators/{operator_name_snake}_operator.hpp`

Use `#ifndef`/`#define`/`#endif` header guards — NOT `#pragma once`.
Pattern: `QUEST_TASKFLOW_OPERATORS_{FILENAME}_HPP_`

### For Streaming (ProcessBatch):

```cpp
// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_NEW_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_NEW_OPERATOR_HPP_

#include <memory>
#include <string>
#include "quest/cache/data_block.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/result.hpp"

namespace quest::taskflow {

/// @brief One-line description of what this operator does.
class NewOperator : public Operator {
 public:
  NewOperator(std::uint32_t id, std::string expression)
      : Operator(id, std::move(expression), OperatorType::NewType) {}

  /// @brief Transform one input batch into one output batch.
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(
      cache::DataBlockPtr input) override;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_NEW_OPERATOR_HPP_
```

### For Source (GetNextBatch):

```cpp
// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_NEW_SOURCE_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_NEW_SOURCE_OPERATOR_HPP_

#include <optional>
#include <memory>
#include "quest/taskflow/operator.hpp"
#include "quest/result.hpp"

namespace quest::taskflow {

/// @brief Source operator: emits batches from an external data source.
class NewSourceOperator : public Operator {
 public:
  explicit NewSourceOperator(std::uint32_t id)
      : Operator(id, "new_source", OperatorType::NewType) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  /// @brief Returns the next batch, or nullopt (EOS) when exhausted.
  /// EOS is signaled as an engaged nullopt — never as Status::Invalid.
  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_NEW_SOURCE_OPERATOR_HPP_
```

### For Blocking/Sink (Sink + FinalizeSink + GetNextBatch):

```cpp
// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_NEW_BLOCKING_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_NEW_BLOCKING_OPERATOR_HPP_

#include <optional>
#include <vector>
#include "quest/taskflow/operator.hpp"
#include "quest/result.hpp"

namespace quest::taskflow {

/// @brief Blocking operator: accumulates state in sink phase, emits in source phase.
/// Appears in two pipelines: sink of the producer pipeline,
/// source of the consumer pipeline (after FinalizeSink flips the state).
class NewBlockingOperator : public Operator {
 public:
  explicit NewBlockingOperator(std::uint32_t id)
      : Operator(id, "new_blocking", OperatorType::NewType) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }
  [[nodiscard]] bool IsSource() const noexcept override { return finalized_; }

  /// @brief Accumulate one incoming batch.
  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override;

  /// @brief Called once when the producer pipeline finishes. Flips to source mode.
  [[nodiscard]] Status FinalizeSink() override;

  /// @brief Emit accumulated batches one at a time; nullopt = EOS.
  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override;

 private:
  std::vector<cache::DataBlockPtr> accumulated_;
  std::size_t emit_index_{0};
  bool finalized_{false};
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_NEW_BLOCKING_OPERATOR_HPP_
```

## Step 4: Create Test File

File: `tests/test_{operator_name_snake}.cpp`

Look at `tests/test_operator_roles.cpp` and `tests/test_pipeline_executor.cpp` for patterns.
Wire with `GraphBuilder` (never with the old `*graph += a >> b` DSL or `link()`).

```cpp
#include <gtest/gtest.h>
#include "quest/taskflow/operators/{operator_name_snake}_operator.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/prepared_query.hpp"

namespace quest::taskflow {
namespace {

TEST({OperatorName}Test, BasicOperation) {
  // Build pipeline via GraphBuilder
  // Assert correct output batches
}

TEST({OperatorName}Test, EmptyInput) {
  // Test with empty DataBlock (0 rows)
}

TEST({OperatorName}Test, EndOfStream) {
  // Verify GetNextBatch/ProcessBatch signals EOS correctly (nullopt, not Status::Invalid)
}

}  // namespace
}  // namespace quest::taskflow
```

## Step 5: Register Test in CMakeLists.txt

File: `tests/CMakeLists.txt`

Add these 3 lines:

```cmake
add_executable(test_{name} test_{name}.cpp)
target_link_libraries(test_{name} PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_{name})
```

## Step 6: Build and Verify

```bash
cmake --build build && ctest --test-dir build
```

## Step 7: Show GraphBuilder Usage Example

Show how to wire the new operator using `GraphBuilder`:

```cpp
// Streaming chain
GraphBuilder gb{memory_manager};
gb.Chain(scan_op, new_op, collector_op);
ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
ASSERT_OK(graph->Execute());

// Multi-input operators use typed ports or named port pairs:
GraphBuilder gb{memory_manager};
gb.Edge(orders_scan, join.probe());
gb.Edge(customers_scan, join.build());
gb.Chain(join, collector_op);
ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
ASSERT_OK(graph->Execute());
```

## Gotchas

- Operators are **non-copyable AND non-movable** (both deleted in base class)
- `owning_graph_` is `weak_ptr<Graph>` — MUST call `.lock()` before use, check for null
- Multi-input operators need named port accessors (`DeclaredInputPorts()` + `InputEdgeContract()`)
- Sentinel node ID is `-1` (`kGraphEntryPointId`)
- Constructor signature is `(uint32_t id, string expression, OperatorType type)`
- Use `shared_ptr` for `DataBlock`, `IBatchHolder`; `unique_ptr` for nothing in operator scope
- **EOS is a value** (`std::optional<DataBlockPtr>` returning `nullopt`) — never signal EOS via `Status::Invalid`
- Header guards use `#ifndef`/`#define`/`#endif` — NOT `#pragma once`
- All files in `src/quest/taskflow/operators/` (not the base `taskflow/` dir for production operators)
