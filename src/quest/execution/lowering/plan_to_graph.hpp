// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file plan_to_graph.hpp
/// @brief Sirius-style create_plan gate: LogicalPlan → push-pipeline Graph.

#include <memory>
#include <utility>
#include <vector>

#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/plan/logical/logical_expr.hpp"
#include "quest/plan/logical/logical_plan.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"

namespace quest::execution {

/// @brief Resolve a logical expression against @p input's schema into an
/// evaluable physical expression.
[[nodiscard]] Result<plan::physical::ExpressionPtr> CreatePhysicalExpr(
    const plan::logical::LogicalExpr& expr, const plan::logical::LogicalPlan& input);

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

/// @brief Translate a supported LogicalPlan into a push-pipeline ExecutableGraph.
///
/// Supported: Scan, Selection, Projection, Limit, Union, Join, Aggregate, Sort (and trees
/// thereof).
/// Unsupported nodes return an error.
[[nodiscard]] Result<ExecutableGraphPtr> CreateGraph(
    const plan::logical::LogicalPlan& plan,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

}  // namespace quest::execution
