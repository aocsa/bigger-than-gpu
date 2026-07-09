// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_EXECUTION_GRAPH_BUILDER_HPP_
#define QUEST_EXECUTION_GRAPH_BUILDER_HPP_

/// @file graph_builder.hpp
/// @brief Sirius-style create_plan gate: LogicalPlan → push-pipeline Graph.

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "quest/logical_plan/logical_plan.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/physical_plan/physical_plan.hpp"
#include "quest/quest.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/operators/filter_operator.hpp"
#include "quest/taskflow/operators/project_operator.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"

namespace quest::execution {

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

[[nodiscard]] inline Status TranslateChain(
    const LogicalPlan& plan, std::shared_ptr<memory::MemoryResourceManager> memory_manager,
    std::vector<std::unique_ptr<taskflow::Operator>>& ops, std::uint32_t& next_id) {
  // Recurse to leaves first so operators are built scan → … → root order.
  if (const auto* scan = dynamic_cast<const Scan*>(&plan)) {
    ops.push_back(std::make_unique<taskflow::TableScanOperator>(
        next_id++, scan->data_source(), scan->projection(), std::move(memory_manager)));
    return Status::OK();
  }

  if (const auto* selection = dynamic_cast<const Selection*>(&plan)) {
    auto child = TranslateChain(selection->input(), memory_manager, ops, next_id);
    if (!child) {
      return child;
    }
    auto expr_result = CreatePhysicalExpr(selection->condition(), selection->input());
    if (!expr_result) {
      return std::unexpected(expr_result.error());
    }
    ops.push_back(std::make_unique<taskflow::FilterOperator>(next_id++, std::move(*expr_result),
                                                             std::move(memory_manager)));
    return Status::OK();
  }

  if (const auto* projection = dynamic_cast<const Projection*>(&plan)) {
    auto child = TranslateChain(projection->input(), memory_manager, ops, next_id);
    if (!child) {
      return child;
    }

    std::vector<ExpressionPtr> exprs;
    exprs.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto expr_result = CreatePhysicalExpr(*expr, projection->input());
      if (!expr_result) {
        return std::unexpected(expr_result.error());
      }
      exprs.push_back(std::move(*expr_result));
    }

    std::vector<datasource::Field> fields;
    fields.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto field_result = expr->ToField(projection->input());
      if (!field_result) {
        return std::unexpected(field_result.error());
      }
      fields.push_back(*field_result);
    }

    ops.push_back(std::make_unique<taskflow::ProjectOperator>(
        next_id++, datasource::Schema(std::move(fields)), std::move(exprs),
        std::move(memory_manager)));
    return Status::OK();
  }

  return Status::PhysicalPlanError("CreateGraph: unsupported logical plan node: " +
                                   plan.ToString());
}

}  // namespace detail

/// @brief Translate a supported LogicalPlan into a push-pipeline ExecutableGraph.
///
/// Supported: Scan, Selection, Projection (and chains thereof). Unsupported
/// nodes return an error — callers fall back to the callback physical engine.
[[nodiscard]] inline Result<ExecutableGraphPtr> CreateGraph(
    const LogicalPlan& plan,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr) {
  if (!memory_manager) {
    memory_manager = make_default_memory_manager();
  }

  std::vector<std::unique_ptr<taskflow::Operator>> ops;
  std::uint32_t next_id = 0;
  auto translated = detail::TranslateChain(plan, memory_manager, ops, next_id);
  if (!translated) {
    return std::unexpected(translated.error());
  }

  auto collector = std::make_unique<taskflow::CollectorOperator>(next_id++);
  auto* collector_ptr = collector.get();
  ops.push_back(std::move(collector));

  auto graph = std::make_shared<taskflow::Graph>(memory_manager);
  // Streaming operators fuse into one pipeline (plain edges) ...
  for (std::size_t i = 0; i + 2 < ops.size(); ++i) {
    *graph += *ops[i] >> *ops[i + 1];
  }
  // ... but the collector sits behind a Full barrier so it becomes its own
  // pipeline (Sirius's RESULT_COLLECTOR shape). This is what makes queries
  // spillable end-to-end: final batches always cross a spill-aware
  // BatchHolder (MaybeSpill runs on Add), whereas a fully fused pipeline
  // has no holder anywhere and could never spill regardless of budget.
  *graph += taskflow::link(*ops[ops.size() - 2], *ops.back(),
                           taskflow::EdgeConfig::Simple().with_barrier(taskflow::BarrierType::Full));

  return std::make_unique<ExecutableGraph>(std::move(graph), std::move(ops), collector_ptr);
}

}  // namespace quest::execution

#endif  // QUEST_EXECUTION_GRAPH_BUILDER_HPP_
