---
name: new-plan-node
description: Adds a new logical + physical plan node and wires it into QueryPlan fluent API. Trigger: "add plan node", "implement Limit", "add Aggregate", "new logical plan"
---

# New Plan Node

Creates a logical plan node, its physical execution counterpart, and wires both into the QueryPlan fluent API.

## Read These Files First

- `src/quest/logical_plan/logical_plan.hpp` — `LogicalPlan` base, `Scan`, `Selection`, `Projection` nodes
- `src/quest/physical_plan/physical_plan.hpp` — `PhysicalPlan` base, exec nodes, `CreatePhysicalPlan()` factory
- `src/quest/builder/query_plan.hpp` — `QueryPlan` fluent API (`Where`, `Select`)
- `src/quest/logical_plan/logical_expr.hpp` — expression types if new expressions needed
- `docs/dev/future_improvement.md` — Priority 4 lists missing plan nodes

## Step 1: Gather Requirements (then STOP)

Ask the user:
1. **Node name** (e.g., "Limit", "Aggregate", "Sort")
2. **Input arity** — unary (one child like Selection) or binary (two children like Join)?
3. **Does it need new expression types?** (e.g., Aggregate needs aggregate expressions)

STOP and wait for answers before proceeding.

## Step 2: Create Logical Plan Node

File: `src/quest/logical_plan/logical_plan.hpp`

Add the new class after the existing nodes (Scan, Selection, Projection). Follow the Selection pattern for unary nodes:

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

## Step 3: Create Physical Plan Node

File: `src/quest/physical_plan/physical_plan.hpp`

Add after existing exec nodes. Follow SelectionExec pattern:

```cpp
/// @brief {NodeName} execution node.
class {NodeName}Exec : public PhysicalPlan {
 public:
  {NodeName}Exec(PhysicalPlanPtr input, /* physical params */)
      : input_(std::move(input)) /* , ... */ {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const PhysicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] Status Execute(const ResultCallback& callback) override {
    return input_->Execute([this, &callback](RecordBatch batch) {
      // Process batch and call callback
    });
  }

  [[nodiscard]] std::string ToString() const override {
    return "{NodeName}Exec(...)";
  }

 private:
  PhysicalPlanPtr input_;
  // Physical node members...
};
```

Key: `Execute()` must propagate errors via `std::expected` (Status), NEVER throw.

## Step 4: Add Branch in CreatePhysicalPlan()

File: `src/quest/physical_plan/physical_plan.hpp`, function `CreatePhysicalPlan()` (line ~439)

Add new `dynamic_cast` branch BEFORE the final error return:

```cpp
if (const auto* node = dynamic_cast<const {NodeName}*>(&plan)) {
  auto input_result = CreatePhysicalPlan(node->input());
  if (!input_result) {
    return std::unexpected(input_result.error());
  }
  // Create physical expressions if needed...
  return std::make_unique<{NodeName}Exec>(std::move(*input_result), /* params */);
}

// This must remain LAST:
return std::unexpected(
    Status::PhysicalPlanError("Unsupported logical plan type"));
```

If the node needs new expression types, also add branches in `CreatePhysicalExpr()` (line ~373).

## Step 5: Add Fluent Method to QueryPlan

File: `src/quest/builder/query_plan.hpp`

Add a new `&&`-qualified method in the Query Building section:

```cpp
/// @brief Add a {node_name} operation.
[[nodiscard]] QueryPlan {MethodName}(/* params */) && {
  return QueryPlan(
      std::make_unique<{NodeName}>(std::move(plan_), /* params */));
}
```

ALL QueryPlan methods are `&&`-qualified — they consume `this` and return a new QueryPlan by value. Chain with `std::move`:

```cpp
auto plan = std::move(base_plan).Where(condition).Select({"col"}).Limit(10);
```

## Step 6: Add Tests

File: `tests/test_logical_plan.cpp` — test logical node creation, schema, ToString

File: `tests/test_physical_plan.cpp` — test physical execution, error propagation

File: `tests/test_query_builder.cpp` — test fluent API integration

## Step 7: Build and Verify

```bash
cmake --build build && ctest --test-dir build
```

## Gotchas

- ALL `QueryPlan` methods are `&&`-qualified — must `std::move` the plan into the new node
- `CreatePhysicalPlan` uses `dynamic_cast` chains — add new branch BEFORE the final error return
- Expressions use `unique_ptr` everywhere — NO `shared_ptr` for expression trees
- Physical `Execute()` must propagate errors via `std::expected`, not exceptions
- `LogicalPlanPtr = unique_ptr<LogicalPlan>`, `PhysicalPlanPtr = unique_ptr<PhysicalPlan>`
- For nodes that change schema (like Aggregate), override `schema()` properly
- Check `docs/dev/future_improvement.md` P4 for implementation order: Limit -> Aggregate -> Sort -> Join
