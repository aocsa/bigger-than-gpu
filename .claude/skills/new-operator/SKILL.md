---
name: new-operator
description: Scaffolds a new QUEST operator with enum entry, class file, test, and CMake registration. Trigger: "add operator", "new operator", "create operator", "implement X operator"
---

# New Operator Scaffold

Creates a complete QUEST operator with all required files and registrations.

## Read These Files First

- `src/quest/taskflow/operator.hpp` — `Operator` and `SimpleOperator` base classes
- `src/quest/taskflow/operator_type.hpp` — `OperatorType` enum and helper functions
- `tests/CMakeLists.txt` — test registration pattern
- `docs/dev/future_improvement.md` — known bugs and missing implementations

## Step 1: Gather Requirements (then STOP)

Ask the user:
1. **Operator name** (e.g., "Sort", "Aggregate")
2. **Stateless or stateful?** — Stateless extends `SimpleOperator` (automatic `run()` loop, just implement `ProcessBatch`). Stateful extends `Operator` (manual `run()` + `can_you_throttle_my_input()`)
3. **Port count** — Single input (default) or multi-input (named ports)?
4. **Brief description** of what it does

STOP and wait for answers before proceeding.

## Step 2: Add Enum Entry

File: `src/quest/taskflow/operator_type.hpp`

1. Add new entry to `OperatorType` enum in the appropriate section (Source/Transformation/Set/Aggregation/Sort/Join/Limit-Output)
2. Add `case` to `to_string()` function
3. If source operator: add to `is_source_operator()`
4. If terminal operator: add to `is_terminal_operator()`
5. If multi-input: add to `is_multi_input_operator()`

## Step 3: Create Operator Class

File: `src/quest/taskflow/{operator_name_snake}.hpp`

### For Stateless (SimpleOperator):

```cpp
#pragma once

#include <memory>
#include <string>
#include "quest/cache/data_block.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/result.hpp"

namespace quest::taskflow {

class NewOperator : public SimpleOperator {
 public:
  NewOperator(std::uint32_t id, std::string expression)
      : SimpleOperator(id, std::move(expression), OperatorType::NewType) {}

 protected:
  [[nodiscard]] Result<std::shared_ptr<cache::DataBlock>> ProcessBatch(
      std::shared_ptr<cache::DataBlock> input) override;
};

}  // namespace quest::taskflow
```

### For Stateful (Operator):

```cpp
#pragma once

#include <memory>
#include <string>
#include "quest/taskflow/operator.hpp"
#include "quest/result.hpp"

namespace quest::taskflow {

class NewOperator : public Operator {
 public:
  NewOperator(std::uint32_t id, std::string expression)
      : Operator(id, std::move(expression), OperatorType::NewType) {}

  [[nodiscard]] ExecutionStatus run() override;
  [[nodiscard]] bool can_you_throttle_my_input() const override;
};

}  // namespace quest::taskflow
```

## Step 4: Create Test File

File: `tests/test_{operator_name_snake}.cpp`

Pattern from existing tests:

```cpp
#include <gtest/gtest.h>
#include "quest/taskflow/{operator_name_snake}.hpp"

namespace quest::taskflow {
namespace {

TEST({OperatorName}Test, BasicOperation) {
  // Test the operator processes data correctly
}

TEST({OperatorName}Test, EmptyInput) {
  // Test with empty DataBlock
}

TEST({OperatorName}Test, NullValues) {
  // Test with null column values
}

}  // namespace
}  // namespace quest::taskflow
```

## Step 5: Register Test in CMakeLists.txt

File: `tests/CMakeLists.txt`

Add these 3 lines at the end:

```cmake
add_executable(test_{name} test_{name}.cpp)
target_link_libraries(test_{name} PRIVATE quest::quest GTest::gtest GTest::gtest_main)
gtest_discover_tests(test_{name})
```

## Step 6: Build and Verify

```bash
cmake --build build && ctest --test-dir build
```

## Step 7: Show DSL Usage Example

Show how to wire the new operator in the graph DSL:

```cpp
auto graph = std::make_shared<Graph>();
*graph += scan >> new_op >> output;
graph->Execute();
```

For multi-input operators:

```cpp
*graph += scan_a >> join["left_input"];
*graph += scan_b >> join["right_input"];
*graph += join >> output;
```

## Gotchas

- Operators are **non-copyable AND non-movable** (both deleted in base class)
- `owning_graph_` is `weak_ptr<Graph>` — MUST call `.lock()` before use, check for null
- Multi-input operators need named ports via `input()["port_name"]`
- Sentinel node ID is `-1` (`kGraphEntryPointId`)
- Constructor signature is `(uint32_t id, string expression, OperatorType type)`
- Use `shared_ptr` for `DataBlock`, `IBatchHolder`. Use `unique_ptr` for nothing in operator scope
- `SimpleOperator::can_you_throttle_my_input()` returns `true` by default
- Known bug: `SimpleOperator::run()` uses `ErrorCode::Invalid` for end-of-stream (see `future_improvement.md` 1.3)
