// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/execution/lowering/plan_to_graph.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/operators/filter_operator.hpp"
#include "quest/taskflow/operators/hash_aggregate_operator.hpp"
#include "quest/taskflow/operators/hash_join_operator.hpp"
#include "quest/taskflow/operators/limit_operator.hpp"
#include "quest/taskflow/operators/project_operator.hpp"
#include "quest/taskflow/operators/sort_operator.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"
#include "quest/taskflow/operators/union_operator.hpp"

namespace quest::execution {

Result<plan::physical::ExpressionPtr> CreatePhysicalExpr(
    const plan::logical::LogicalExpr& expr, const plan::logical::LogicalPlan& input) {
  if (const auto* literal = dynamic_cast<const plan::logical::Literal*>(&expr)) {
    return std::make_unique<plan::physical::LiteralExpression>(literal->value());
  }

  if (const auto* col_idx = dynamic_cast<const plan::logical::ColumnIndex*>(&expr)) {
    return std::make_unique<plan::physical::ColumnExpression>(col_idx->index());
  }

  if (const auto* col = dynamic_cast<const plan::logical::Column*>(&expr)) {
    auto index = input.schema().GetFieldIndex(col->name());
    if (!index) {
      return Error::SchemaError("No column named: " + col->name());
    }
    return std::make_unique<plan::physical::ColumnExpression>(*index);
  }

  if (const auto* binary = dynamic_cast<const plan::logical::BinaryExpr*>(&expr)) {
    auto left_result = CreatePhysicalExpr(binary->left(), input);
    if (!left_result) {
      return std::unexpected(left_result.error());
    }
    auto right_result = CreatePhysicalExpr(binary->right(), input);
    if (!right_result) {
      return std::unexpected(right_result.error());
    }

    return std::make_unique<plan::physical::BinaryExpression>(
        binary->op(), std::move(*left_result), std::move(*right_result));
  }

  if (const auto* case_expr = dynamic_cast<const plan::logical::CaseExpr*>(&expr)) {
    plan::physical::CaseExpression::Branches branches;
    branches.reserve(case_expr->branches().size());
    for (const auto& [when, then] : case_expr->branches()) {
      auto when_result = CreatePhysicalExpr(*when, input);
      if (!when_result) {
        return std::unexpected(when_result.error());
      }
      auto then_result = CreatePhysicalExpr(*then, input);
      if (!then_result) {
        return std::unexpected(then_result.error());
      }
      branches.emplace_back(std::move(*when_result), std::move(*then_result));
    }
    auto otherwise_result = CreatePhysicalExpr(case_expr->otherwise(), input);
    if (!otherwise_result) {
      return std::unexpected(otherwise_result.error());
    }
    return std::make_unique<plan::physical::CaseExpression>(std::move(branches),
                                                            std::move(*otherwise_result));
  }

  return Error::PhysicalPlanError("Cannot create physical expression from: " + expr.ToString());
}

namespace {

/// @brief Lowers one `name = AGG(expr)` of an Aggregate node to the operator's spec.
[[nodiscard]] Result<taskflow::AggregateSpec> LowerAggregate(
    const plan::logical::NamedAggregate& aggregate, const plan::logical::LogicalPlan& input) {
  const auto* call = dynamic_cast<const plan::logical::AggregateExpr*>(aggregate.expr.get());
  if (call == nullptr) {
    return Error::PhysicalPlanError("Aggregate output '" + aggregate.name +
                                    "' is not an aggregate function: " +
                                    aggregate.expr->ToString());
  }
  if (call->kind() == plan::AggregateKind::kCountStar) {
    return taskflow::AggregateSpec{call->kind(), nullptr, aggregate.name,
                                   columnar::DataType::kInt64};
  }
  auto input_expr = CreatePhysicalExpr(*call->input(), input);
  if (!input_expr) {
    return std::unexpected(std::move(input_expr).error());
  }
  auto field = call->ToField(input);
  if (!field) {
    return std::unexpected(std::move(field).error());
  }
  return taskflow::AggregateSpec{call->kind(), std::move(*input_expr), aggregate.name,
                                 field->data_type};
}

/// @brief If every expression of @p projection is a pure column reference
/// (`Column` by name, or `ColumnIndex` resolved against @p input's schema),
/// returns the ordered projected column names; otherwise `std::nullopt`.
///
/// This is the BindableTableScan test: a pure column-selection projection can be
/// folded into the scan (read only these columns), but any computation
/// (arithmetic, literals) must stay a separate ProjectOperator.
[[nodiscard]] std::optional<std::vector<std::string>> TryColumnRefProjection(
    const plan::logical::Projection& projection, const plan::logical::LogicalPlan& input) {
  const auto input_schema = input.schema();
  std::vector<std::string> columns;
  columns.reserve(projection.expressions().size());
  for (const auto& expr : projection.expressions()) {
    if (const auto* col = dynamic_cast<const plan::logical::Column*>(expr.get())) {
      columns.push_back(col->name());
    } else if (const auto* col_idx = dynamic_cast<const plan::logical::ColumnIndex*>(expr.get())) {
      if (col_idx->index() >= input_schema.num_fields()) {
        return std::nullopt;
      }
      columns.push_back(input_schema.field(col_idx->index()).name);
    } else {
      return std::nullopt;  // computation — not a pure column selection.
    }
  }
  return columns;
}

/// @brief Translates one logical plan into taskflow operators that it owns and numbers.
///
/// `Translate` recurses to leaves first (scan → … → root) and wires each subtree's internal
/// edges as it returns. Branching nodes (Union, Join) translate each child subtree and wire
/// both roots into their named ports, which a flat post-pass over the operators cannot do.
class Lowering {
 public:
  explicit Lowering(std::shared_ptr<memory::MemoryResourceManager> memory)
      : memory_(std::move(memory)), gb_{memory_} {}

  /// @return The root operator of @p plan, whose output feeds the parent. The pointer is
  /// stable: the operator never moves, only its owning unique_ptr does.
  [[nodiscard]] Result<taskflow::Operator*> Translate(const plan::logical::LogicalPlan& plan);

  /// @brief Ends the graph at a CollectorOperator fed by @p root and builds it.
  [[nodiscard]] Result<ExecutableGraphPtr> Finish(taskflow::Operator& root) && {
    // CollectorOperator declares Full via InputEdgeContract, so the root → collector edge
    // infers Full (the RESULT_COLLECTOR shape) without a special-case link() call.
    auto& collector = Add<taskflow::CollectorOperator>();
    gb_.Edge(root, collector);
    return std::move(gb_).Build().transform([&](std::shared_ptr<taskflow::Graph> graph) {
      return std::make_unique<ExecutableGraph>(std::move(graph), std::move(ops_), &collector);
    });
  }

 private:
  /// @brief Makes an `Op` with the next operator id followed by @p args, and keeps it.
  template <typename Op, typename... Args>
  Op& Add(Args&&... args) {
    auto& op = ops_.emplace_back(std::make_unique<Op>(next_id_++, std::forward<Args>(args)...));
    return static_cast<Op&>(*op);
  }

  std::shared_ptr<memory::MemoryResourceManager> memory_;
  taskflow::GraphBuilder gb_;
  std::vector<std::unique_ptr<taskflow::Operator>> ops_;
  std::uint32_t next_id_ = 0;
};

Result<taskflow::Operator*> Lowering::Translate(const plan::logical::LogicalPlan& plan) {
  if (const auto* scan = dynamic_cast<const plan::logical::Scan*>(&plan)) {
    return &Add<taskflow::TableScanOperator>(scan->data_source(), scan->projection(), memory_);
  }

  if (const auto* selection = dynamic_cast<const plan::logical::Selection*>(&plan)) {
    auto child = Translate(selection->input());
    if (!child) {
      return std::unexpected(child.error());
    }
    auto expr_result = CreatePhysicalExpr(selection->condition(), selection->input());
    if (!expr_result) {
      return std::unexpected(expr_result.error());
    }
    auto& op = Add<taskflow::FilterOperator>(std::move(*expr_result), memory_);
    gb_.Edge(**child, op);
    return &op;
  }

  if (const auto* projection = dynamic_cast<const plan::logical::Projection*>(&plan)) {
    // BindableTableScan fold (F3): Projection(Scan) whose expressions are all
    // pure column references collapses into ONE scan that reads just those
    // columns — the ProjectOperator disappears and fewer columns come off disk.
    if (const auto* scan = dynamic_cast<const plan::logical::Scan*>(&projection->input())) {
      if (auto folded = TryColumnRefProjection(*projection, *scan)) {
        auto& op = Add<taskflow::TableScanOperator>(scan->data_source(), std::move(*folded),
                                                    memory_);
        op.set_type(taskflow::OperatorType::BindableTableScan);
        return &op;
      }
    }

    auto child = Translate(projection->input());
    if (!child) {
      return std::unexpected(child.error());
    }

    std::vector<plan::physical::ExpressionPtr> exprs;
    exprs.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto expr_result = CreatePhysicalExpr(*expr, projection->input());
      if (!expr_result) {
        return std::unexpected(expr_result.error());
      }
      exprs.push_back(std::move(*expr_result));
    }

    std::vector<columnar::Field> fields;
    fields.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto field_result = expr->ToField(projection->input());
      if (!field_result) {
        return std::unexpected(field_result.error());
      }
      fields.push_back(*field_result);
    }

    auto& op = Add<taskflow::ProjectOperator>(columnar::Schema(std::move(fields)),
                                              std::move(exprs), memory_);
    gb_.Edge(**child, op);
    return &op;
  }

  if (const auto* limit = dynamic_cast<const plan::logical::Limit*>(&plan)) {
    auto child = Translate(limit->input());
    if (!child) {
      return std::unexpected(child.error());
    }
    auto& op = Add<taskflow::LimitOperator>(limit->limit(), memory_);
    gb_.Edge(**child, op);
    return &op;
  }

  if (const auto* union_all = dynamic_cast<const plan::logical::Union*>(&plan)) {
    if (!plan::logical::UnionCompatible(union_all->left().schema(), union_all->right().schema())) {
      return Error::SchemaError("UNION ALL inputs have mismatched schemas");
    }
    auto left = Translate(union_all->left());
    if (!left) {
      return std::unexpected(left.error());
    }
    auto right = Translate(union_all->right());
    if (!right) {
      return std::unexpected(right.error());
    }
    auto& op = Add<taskflow::UnionOperator>();
    gb_.Edge(**left, op.input_a());
    gb_.Edge(**right, op.input_b());
    return &op;
  }

  if (const auto* join = dynamic_cast<const plan::logical::Join*>(&plan)) {
    auto probe = Translate(join->probe());
    if (!probe) {
      return std::unexpected(probe.error());
    }
    auto build = Translate(join->build());
    if (!build) {
      return std::unexpected(build.error());
    }
    auto& op = Add<taskflow::HashJoinOperator>(join->build_key(), join->probe_key(), memory_);
    gb_.Edge(**build, op.build());
    gb_.Edge(**probe, op.probe());
    return &op;
  }

  if (const auto* aggregate = dynamic_cast<const plan::logical::Aggregate*>(&plan)) {
    auto child = Translate(aggregate->input());
    if (!child) {
      return std::unexpected(child.error());
    }
    std::vector<taskflow::AggregateSpec> specs;
    specs.reserve(aggregate->aggregates().size());
    for (const auto& named : aggregate->aggregates()) {
      auto spec = LowerAggregate(named, aggregate->input());
      if (!spec) {
        return std::unexpected(std::move(spec).error());
      }
      specs.push_back(std::move(*spec));
    }
    auto& op =
        Add<taskflow::HashAggregateOperator>(aggregate->group_by(), std::move(specs), memory_);
    gb_.Edge(**child, op);
    return &op;
  }

  if (const auto* sort = dynamic_cast<const plan::logical::Sort*>(&plan)) {
    auto child = Translate(sort->input());
    if (!child) {
      return std::unexpected(child.error());
    }
    std::vector<taskflow::SortKey> keys;
    keys.reserve(sort->keys().size());
    for (const auto& key : sort->keys()) {
      keys.push_back({key.column, key.ascending});
    }
    auto& op = Add<taskflow::SortOperator>(std::move(keys), memory_);
    gb_.Edge(**child, op);
    return &op;
  }

  return Error::PhysicalPlanError("CreateGraph: unsupported logical plan node: " +
                                  plan.ToString());
}

}  // namespace

Result<ExecutableGraphPtr> CreateGraph(
    const plan::logical::LogicalPlan& plan,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager) {
  if (!memory_manager) {
    memory_manager = std::make_shared<memory::MemoryResourceManager>(
        std::make_shared<memory::CPUMemoryResource>(),
        std::make_shared<memory::DiskMemoryResource>());
  }
  Lowering lowering(std::move(memory_manager));
  return lowering.Translate(plan).and_then(
      [&](taskflow::Operator* root) { return std::move(lowering).Finish(*root); });
}

}  // namespace quest::execution
