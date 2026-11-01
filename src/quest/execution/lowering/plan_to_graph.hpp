// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file plan_to_graph.hpp
/// @brief Sirius-style create_plan gate: LogicalPlan → push-pipeline Graph.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/plan/logical/logical_expr.hpp"
#include "quest/plan/logical/logical_plan.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/operators/filter_operator.hpp"
#include "quest/taskflow/operators/limit_operator.hpp"
#include "quest/taskflow/operators/project_operator.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"
#include "quest/taskflow/operators/union_operator.hpp"

namespace quest::execution {

/// @brief Resolve a logical expression against @p input's schema into an
/// evaluable physical expression.
[[nodiscard]] inline Result<plan::physical::ExpressionPtr> CreatePhysicalExpr(
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
      return std::unexpected(Error::SchemaError("No column named: " + col->name()));
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

    auto left = std::move(*left_result);
    auto right = std::move(*right_result);

    if (dynamic_cast<const plan::logical::Eq*>(binary)) {
      return std::make_unique<plan::physical::EqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Neq*>(binary)) {
      return std::make_unique<plan::physical::NeqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Lt*>(binary)) {
      return std::make_unique<plan::physical::LtExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::LtEq*>(binary)) {
      return std::make_unique<plan::physical::LtEqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Gt*>(binary)) {
      return std::make_unique<plan::physical::GtExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::GtEq*>(binary)) {
      return std::make_unique<plan::physical::GtEqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::And*>(binary)) {
      return std::make_unique<plan::physical::AndExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Or*>(binary)) {
      return std::make_unique<plan::physical::OrExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Add*>(binary)) {
      return std::make_unique<plan::physical::AddExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Subtract*>(binary)) {
      return std::make_unique<plan::physical::SubtractExpression>(std::move(left),
                                                                  std::move(right));
    }
    if (dynamic_cast<const plan::logical::Multiply*>(binary)) {
      return std::make_unique<plan::physical::MultiplyExpression>(std::move(left),
                                                                  std::move(right));
    }
    if (dynamic_cast<const plan::logical::Divide*>(binary)) {
      return std::make_unique<plan::physical::DivideExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const plan::logical::Modulo*>(binary)) {
      return std::make_unique<plan::physical::ModuloExpression>(std::move(left), std::move(right));
    }
  }

  return std::unexpected(
      Error::PhysicalPlanError("Cannot create physical expression from: " + expr.ToString()));
}

/// @brief Owns operators + Graph so Operator* edges stay valid for Execute().
class ExecutableGraph {
 public:
  ExecutableGraph(std::shared_ptr<taskflow::Graph> graph,
                  std::vector<std::unique_ptr<taskflow::Operator>> operators,
                  taskflow::CollectorOperator* collector)
      : graph_(std::move(graph)), operators_(std::move(operators)), collector_(collector) {}

  [[nodiscard]] taskflow::Graph& graph() noexcept { return *graph_; }
  [[nodiscard]] const taskflow::Graph& graph() const noexcept { return *graph_; }

  [[nodiscard]] Status Execute() { return graph_->Execute(); }

  [[nodiscard]] taskflow::CollectorOperator& collector() noexcept { return *collector_; }
  [[nodiscard]] const taskflow::CollectorOperator& collector() const noexcept {
    return *collector_;
  }

 private:
  std::shared_ptr<taskflow::Graph> graph_;
  std::vector<std::unique_ptr<taskflow::Operator>> operators_;
  taskflow::CollectorOperator* collector_;
};

using ExecutableGraphPtr = std::unique_ptr<ExecutableGraph>;

namespace detail {

/// @brief If every expression of @p projection is a pure column reference
/// (`Column` by name, or `ColumnIndex` resolved against @p input's schema),
/// returns the ordered projected column names; otherwise `std::nullopt`.
///
/// This is the BindableTableScan test: a pure column-selection projection can be
/// folded into the scan (read only these columns), but any computation
/// (arithmetic, literals) must stay a separate ProjectOperator.
[[nodiscard]] inline std::optional<std::vector<std::string>> TryColumnRefProjection(
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

/// @brief Translate a logical subtree into taskflow operators, wiring the
/// subtree's internal edges into @p gb, and return its root operator (whose
/// output feeds the parent).
///
/// Recurses to leaves first (scan → … → root). Each operator is owned by
/// @p ops; the returned pointer is stable (the object never moves, only the
/// owning unique_ptr does). Branching nodes (Union) translate each child
/// subtree and wire both roots into their named ports — which is why wiring
/// lives here rather than in a flat post-pass over @p ops.
[[nodiscard]] inline Result<taskflow::Operator*> TranslateNode(
    const plan::logical::LogicalPlan& plan,
    const std::shared_ptr<memory::MemoryResourceManager>& memory_manager,
    std::vector<std::unique_ptr<taskflow::Operator>>& ops, taskflow::GraphBuilder& gb,
    std::uint32_t& next_id) {
  if (const auto* scan = dynamic_cast<const plan::logical::Scan*>(&plan)) {
    auto op = std::make_unique<taskflow::TableScanOperator>(
        next_id++, scan->data_source(), scan->projection(), memory_manager);
    auto* op_ptr = op.get();
    ops.push_back(std::move(op));
    return op_ptr;
  }

  if (const auto* selection = dynamic_cast<const plan::logical::Selection*>(&plan)) {
    auto child = TranslateNode(selection->input(), memory_manager, ops, gb, next_id);
    if (!child) {
      return std::unexpected(child.error());
    }
    auto expr_result = CreatePhysicalExpr(selection->condition(), selection->input());
    if (!expr_result) {
      return std::unexpected(expr_result.error());
    }
    auto op = std::make_unique<taskflow::FilterOperator>(next_id++, std::move(*expr_result),
                                                         memory_manager);
    auto* op_ptr = op.get();
    ops.push_back(std::move(op));
    gb.Edge(**child, *op_ptr);
    return op_ptr;
  }

  if (const auto* projection = dynamic_cast<const plan::logical::Projection*>(&plan)) {
    // BindableTableScan fold (F3): Projection(Scan) whose expressions are all
    // pure column references collapses into ONE scan that reads just those
    // columns — the ProjectOperator disappears and fewer columns come off disk.
    if (const auto* scan = dynamic_cast<const plan::logical::Scan*>(&projection->input())) {
      if (auto folded = TryColumnRefProjection(*projection, *scan)) {
        auto op = std::make_unique<taskflow::TableScanOperator>(
            next_id++, scan->data_source(), std::move(*folded), memory_manager);
        op->set_type(taskflow::OperatorType::BindableTableScan);
        auto* op_ptr = op.get();
        ops.push_back(std::move(op));
        return op_ptr;
      }
    }

    auto child = TranslateNode(projection->input(), memory_manager, ops, gb, next_id);
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

    auto op = std::make_unique<taskflow::ProjectOperator>(
        next_id++, columnar::Schema(std::move(fields)), std::move(exprs), memory_manager);
    auto* op_ptr = op.get();
    ops.push_back(std::move(op));
    gb.Edge(**child, *op_ptr);
    return op_ptr;
  }

  if (const auto* limit = dynamic_cast<const plan::logical::Limit*>(&plan)) {
    auto child = TranslateNode(limit->input(), memory_manager, ops, gb, next_id);
    if (!child) {
      return std::unexpected(child.error());
    }
    auto op = std::make_unique<taskflow::LimitOperator>(next_id++, limit->limit(), memory_manager);
    auto* op_ptr = op.get();
    ops.push_back(std::move(op));
    gb.Edge(**child, *op_ptr);
    return op_ptr;
  }

  if (const auto* union_all = dynamic_cast<const plan::logical::Union*>(&plan)) {
    if (!plan::logical::UnionCompatible(union_all->left().schema(), union_all->right().schema())) {
      return std::unexpected(Error::SchemaError("UNION ALL inputs have mismatched schemas"));
    }
    auto left = TranslateNode(union_all->left(), memory_manager, ops, gb, next_id);
    if (!left) {
      return std::unexpected(left.error());
    }
    auto right = TranslateNode(union_all->right(), memory_manager, ops, gb, next_id);
    if (!right) {
      return std::unexpected(right.error());
    }
    auto op = std::make_unique<taskflow::UnionOperator>(next_id++);
    auto* op_ptr = op.get();
    ops.push_back(std::move(op));
    gb.Edge(**left, op_ptr->input_a());
    gb.Edge(**right, op_ptr->input_b());
    return op_ptr;
  }

  return std::unexpected(Error::PhysicalPlanError(
      "CreateGraph: unsupported logical plan node: " + plan.ToString()));
}

}  // namespace detail

/// @brief Translate a supported LogicalPlan into a push-pipeline ExecutableGraph.
///
/// Supported: Scan, Selection, Projection, Limit, Union (and trees thereof).
/// Unsupported nodes return an error.
[[nodiscard]] inline Result<ExecutableGraphPtr> CreateGraph(
    const plan::logical::LogicalPlan& plan,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr) {
  if (!memory_manager) {
    memory_manager = std::make_shared<memory::MemoryResourceManager>(
        std::make_shared<memory::CPUMemoryResource>(),
        std::make_shared<memory::DiskMemoryResource>());
  }

  // TranslateNode wires internal edges as it recurses (branching nodes like
  // Union need per-port wiring, which a flat post-pass can't express) and
  // returns the plan's root operator.
  std::vector<std::unique_ptr<taskflow::Operator>> ops;
  std::uint32_t next_id = 0;
  taskflow::GraphBuilder gb{memory_manager};
  auto root = detail::TranslateNode(plan, memory_manager, ops, gb, next_id);
  if (!root) {
    return std::unexpected(root.error());
  }

  // CollectorOperator declares Full via InputEdgeContract, so the root →
  // collector edge automatically infers Full — the RESULT_COLLECTOR shape —
  // without a special-case link() call.
  auto collector = std::make_unique<taskflow::CollectorOperator>(next_id++);
  auto* collector_ptr = collector.get();
  gb.Edge(**root, *collector);
  ops.push_back(std::move(collector));

  auto graph_result = std::move(gb).Build();
  if (!graph_result) {
    return std::unexpected(graph_result.error());
  }

  return std::make_unique<ExecutableGraph>(std::move(*graph_result), std::move(ops), collector_ptr);
}

}  // namespace quest::execution
