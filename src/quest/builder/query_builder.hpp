// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file query_builder.hpp
/// @brief Fluent query builder; ExecuteOnGraph() runs the query on the taskflow engine.

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/builder/query_plan.hpp"
#include "quest/execution/lowering/plan_to_graph.hpp"
#include "quest/memory/memory_resource.hpp"

namespace quest {

// =============================================================================
// Query Builder
// =============================================================================

/// @brief Fluent query builder executed on the taskflow Graph engine.
///
/// Example:
/// @code
///   auto result = QueryBuilder::FromParquet("file.parquet");
///   if (!result) { /* handle error */ }
///
///   auto batches = std::move(result.value())
///       .Where(Col("salary") > Lit(50000))
///       .Select({"id", "name", "salary"})
///       .ExecuteOnGraph();
/// @endcode
class QueryBuilder {
 public:
  // Rule of Five: Move-only (plan ownership)
  QueryBuilder(QueryBuilder&&) noexcept = default;
  QueryBuilder& operator=(QueryBuilder&&) noexcept = default;
  QueryBuilder(const QueryBuilder&) = delete;
  QueryBuilder& operator=(const QueryBuilder&) = delete;
  ~QueryBuilder() = default;

  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  /// @brief Start with a Parquet data source.
  [[nodiscard]] static Result<QueryBuilder> FromParquet(const std::string& filename) {
    auto plan_result = QueryPlan::FromParquet(filename);
    if (!plan_result) {
      return std::unexpected(plan_result.error());
    }
    return QueryBuilder(std::move(*plan_result));
  }

  /// @brief Start with a generic data source.
  [[nodiscard]] static QueryBuilder From(std::shared_ptr<datasource::DataSource> source,
                                         const std::string& name) {
    return QueryBuilder(QueryPlan::From(std::move(source), name));
  }

  // ===========================================================================
  // Query Building (Fluent Interface)
  // ===========================================================================

  [[nodiscard]] QueryBuilder Where(const ExprBuilder& condition) && {
    return QueryBuilder(std::move(plan_).Where(condition));
  }

  [[nodiscard]] QueryBuilder Select(std::initializer_list<std::string> columns) && {
    return QueryBuilder(std::move(plan_).Select(columns));
  }

  [[nodiscard]] QueryBuilder Select(std::initializer_list<ExprBuilder> exprs) && {
    return QueryBuilder(std::move(plan_).Select(exprs));
  }

  [[nodiscard]] QueryBuilder Select(const std::vector<ExprBuilder>& exprs) && {
    return QueryBuilder(std::move(plan_).Select(exprs));
  }

  /// @brief Truncate the result to the first @p n rows.
  [[nodiscard]] QueryBuilder Limit(std::size_t n) && {
    return QueryBuilder(std::move(plan_).Limit(n));
  }

  /// @brief Concatenate this query with @p other (UNION ALL; same schema).
  [[nodiscard]] QueryBuilder UnionAll(QueryBuilder other) && {
    return QueryBuilder(std::move(plan_).UnionAll(std::move(other.plan_)));
  }

  // ===========================================================================
  // Plan Building
  // ===========================================================================

  [[nodiscard]] plan::logical::LogicalPlanPtr BuildLogicalPlan() && {
    return std::move(plan_).BuildLogicalPlan();
  }

  // ===========================================================================
  // Execution
  // ===========================================================================

  /// @brief Execute via the push-pipeline Graph engine.
  ///
  /// Lowers the plan with `CreateGraph` and runs `Graph::Execute()` (PipelineExecutor).
  ///
  /// @param memory_manager Optional memory manager (spill budget).
  /// @return Collected RecordBatches, or the lowering/execution error.
  [[nodiscard]] Result<std::vector<columnar::RecordBatch>> ExecuteOnGraph(
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr) && {
    auto graph_result = execution::CreateGraph(plan_.plan(), std::move(memory_manager));
    if (!graph_result) {
      return std::unexpected(graph_result.error());
    }
    auto exec = std::move(*graph_result);
    auto status = exec->Execute();
    if (!status) {
      return std::unexpected(status.error());
    }
    return exec->collector().CollectBatches();
  }

  // ===========================================================================
  // Utility Methods
  // ===========================================================================

  [[nodiscard]] std::string ToString() const { return plan_.ToString(); }

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(plan_); }

  [[nodiscard]] columnar::Schema schema() const { return plan_.schema(); }

 private:
  explicit QueryBuilder(QueryPlan plan) noexcept : plan_(std::move(plan)) {}

  QueryPlan plan_;
};

}  // namespace quest
