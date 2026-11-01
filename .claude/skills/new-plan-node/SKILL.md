---
name: new-plan-node
description: Adds a new logical plan node, lowers it to a taskflow operator, and wires it into QueryPlan fluent API. Trigger: "add plan node", "implement Limit", "add Aggregate", "new logical plan"
---

# New Plan Node

Creates a logical plan node, lowers it to a taskflow operator in `CreateGraph`, and wires it into
the QueryPlan fluent API. `QueryBuilder::ExecuteOnGraph` is the only execution path.

## Read These Files First

- `src/quest/plan/logical/logical_plan.hpp` (+ `.cpp`) — `LogicalPlan` base, `Scan`, `Selection`, `Projection`, `Limit`, `Union` nodes
- `src/quest/execution/lowering/plan_to_graph.cpp` — `TranslateNode()` (logical node → taskflow operator) and `CreatePhysicalExpr()`
- `src/quest/taskflow/operators/` — existing operators the lowering creates
- `src/quest/builder/query_plan.hpp` — `QueryPlan` fluent API (`Where`, `Select`, `Limit`, `UnionAll`); `query_builder.hpp` forwards each method
- `src/quest/plan/logical/logical_expr.hpp` — expression types if new expressions needed
- `tasks/todo.md` — roadmap; Aggregate, Sort and Join operators are deferred to phase 6

## Step 1: Gather Requirements (then STOP)

Ask the user:
1. **Node name** (e.g., "Limit", "Aggregate", "Sort")
2. **Input arity** — unary (one child like Selection) or binary (two children like Join)?
3. **Does it need new expression types?** (e.g., Aggregate needs aggregate expressions)

STOP and wait for answers before proceeding.

## Step 2: Create Logical Plan Node

File: `src/quest/plan/logical/logical_plan.hpp` (namespace `quest::plan::logical`)

Add the new class after the existing nodes. Follow the Limit pattern for unary nodes (Union for
binary); put non-trivial bodies in `logical_plan.cpp`:

```cpp
/// @brief {NodeName} node - {description}.
class {NodeName} : public LogicalPlan {
 public:
  {NodeName}(LogicalPlanPtr input, /* node-specific params */)
      : input_(std::move(input)) /* , ... */ {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
    return "{NodeName}: " + /* params */;
  }

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  // Accessors for node-specific params...

 private:
  LogicalPlanPtr input_;
  // Node-specific members...
};
```

Key ownership: `LogicalPlanPtr = std::unique_ptr<LogicalPlan>` for child nodes, `LogicalExprPtr = std::unique_ptr<LogicalExpr>` for expressions.

## Step 3: Provide the Taskflow Operator

The node executes as a taskflow operator in `src/quest/taskflow/operators/`. If no existing
operator does the work, scaffold one first with the `new-operator` skill (`/project:new-op`) and
come back. The `LimitOperator` is the model for a unary streaming node; `UnionOperator` for a
node with two inputs (named ports `input_a` / `input_b`).

## Step 4: Add a Lowering Branch in TranslateNode()

File: `src/quest/execution/lowering/plan_to_graph.cpp`, function `TranslateNode()` (anonymous
namespace)

Add a new `dynamic_cast` branch BEFORE the final error return. Follow the Limit pattern:
translate the child first, create the operator, hand ownership to `ops`, wire the child's root
into it with `gb.Edge`, and return the new root:

```cpp
if (const auto* node = dynamic_cast<const plan::logical::{NodeName}*>(&plan)) {
  auto child = TranslateNode(node->input(), memory_manager, ops, gb, next_id);
  if (!child) {
    return std::unexpected(child.error());
  }
  // Create physical expressions if needed: CreatePhysicalExpr(expr, node->input())
  auto op = std::make_unique<taskflow::{NodeName}Operator>(next_id++, /* params */);
  auto* op_ptr = op.get();
  ops.push_back(std::move(op));
  gb.Edge(**child, *op_ptr);
  return op_ptr;
}

// This must remain LAST:
return std::unexpected(Error::PhysicalPlanError(
    "CreateGraph: unsupported logical plan node: " + plan.ToString()));
```

Include the operator header at the top of the file, and add the node to the "Supported:" list
in the `CreateGraph` doc comment in `plan_to_graph.hpp`.

If the node needs new expression types, add the physical class in
`src/quest/plan/physical/expressions/physical_expr.{hpp,cpp}` and a branch in
`CreatePhysicalExpr()` (same `.cpp` file, above `TranslateNode`). Comparison and arithmetic
operators already exist: extend `plan::BinaryOp` (`src/quest/plan/binary_op.hpp`) instead of
adding new expression classes for them.

## Step 5: Add Fluent Method to QueryPlan

File: `src/quest/builder/query_plan.hpp`

Add a new `&&`-qualified method in the Query Building section:

```cpp
/// @brief Add a {node_name} operation.
[[nodiscard]] QueryPlan {MethodName}(/* params */) && {
  return QueryPlan(
      std::make_unique<plan::logical::{NodeName}>(std::move(plan_), /* params */));
}
```

Add the matching forwarding method to `QueryBuilder` in `src/quest/builder/query_builder.hpp`
(`return QueryBuilder(std::move(plan_).{MethodName}(...));`).

ALL QueryPlan methods are `&&`-qualified — they consume `this` and return a new QueryPlan by value. Chain with `std::move`:

```cpp
auto plan = std::move(base_plan).Where(condition).Select({"col"}).Limit(10);
```

## Step 6: Add Tests

File: `tests/test_logical_plan.cpp` — test logical node creation, schema, ToString

File: `tests/test_{node_name_snake}.cpp` — test `CreateGraph` lowering and `ExecuteOnGraph` results, error propagation (see `tests/test_limit.cpp`); register it in `tests/CMakeLists.txt`

File: `tests/test_query_builder.cpp` — test fluent API integration

## Step 7: Build and Verify

```bash
cmake --build build && ctest --test-dir build
```

## Gotchas

- ALL `QueryPlan` methods are `&&`-qualified — must `std::move` the plan into the new node
- `TranslateNode` uses `dynamic_cast` chains — add new branch BEFORE the final error return
- Expressions use `unique_ptr` everywhere — NO `shared_ptr` for expression trees
- Lowering propagates errors via `std::expected` (`return std::unexpected(r.error());`), not exceptions
- `LogicalPlanPtr = unique_ptr<LogicalPlan>`; operators are owned by `ExecutableGraph` (the `ops` vector), the graph holds `Operator*`
- A `Projection` of pure column references directly over a `Scan` is folded into one `BindableTableScan`; new nodes between them disable that fold
- For nodes that change schema (like Aggregate), override `schema()` properly
- Aggregate, Sort and Join have `OperatorType` entries but no operator class yet; Step 3 must create one
