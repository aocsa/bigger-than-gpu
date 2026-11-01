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
class GroupedQueryBuilder;

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
    return QueryPlan::FromParquet(filename).transform(
        [](QueryPlan plan) { return QueryBuilder(std::move(plan)); });
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

  /// @brief Inner join on probe_key = build_key. This query is the probe side
  ///        (streams); @p build is materialized into a hash table.
  [[nodiscard]] QueryBuilder Join(QueryBuilder build, std::string probe_key,
                                  std::string build_key) && {
    return QueryBuilder(
        std::move(plan_).Join(std::move(build.plan_), std::move(probe_key), std::move(build_key)));
  }

  /// @brief Start a GROUP BY; finish it with `.Aggregate({...})`.
  [[nodiscard]] GroupedQueryBuilder GroupBy(std::vector<std::string> columns) &&;

  /// @brief ORDER BY @p columns, ascending.
  [[nodiscard]] QueryBuilder OrderBy(std::initializer_list<std::string> columns) && {
    std::vector<plan::logical::SortKey> keys;
    keys.reserve(columns.size());
    for (const auto& column : columns) {
      keys.push_back({column, true});
    }
    return QueryBuilder(std::move(plan_).OrderBy(std::move(keys)));
  }

  [[nodiscard]] QueryBuilder OrderBy(std::vector<plan::logical::SortKey> keys) && {
    return QueryBuilder(std::move(plan_).OrderBy(std::move(keys)));
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
    return execution::CreateGraph(plan_.plan(), std::move(memory_manager))
        .and_then([](execution::ExecutableGraphPtr exec) {
          return exec->Execute().and_then([&] { return exec->collector().CollectBatches(); });
        });
  }

  // ===========================================================================
  // Utility Methods
  // ===========================================================================

  [[nodiscard]] std::string ToString() const { return plan_.ToString(); }

  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(plan_); }

  [[nodiscard]] columnar::Schema schema() const { return plan_.schema(); }

 private:
  explicit QueryBuilder(QueryPlan plan) noexcept : plan_(std::move(plan)) {}

  friend class GroupedQueryBuilder;

  QueryPlan plan_;
};

// =============================================================================
// Grouped Query Builder
// =============================================================================

/// @brief The pending GROUP BY of `QueryBuilder::GroupBy()`; only `Aggregate()` continues it.
class GroupedQueryBuilder {
 public:
  /// @param aggregates (output name, aggregate), e.g. `{"n", agg::CountStar()}`.
  [[nodiscard]] QueryBuilder Aggregate(
      const std::vector<std::pair<std::string, ExprBuilder>>& aggregates) && {
    return QueryBuilder(
        std::move(query_.plan_).Aggregate(std::move(group_by_), aggregates));
  }

 private:
  friend class QueryBuilder;

  GroupedQueryBuilder(QueryBuilder query, std::vector<std::string> group_by)
      : query_(std::move(query)), group_by_(std::move(group_by)) {}

  QueryBuilder query_;
  std::vector<std::string> group_by_;
};

inline GroupedQueryBuilder QueryBuilder::GroupBy(std::vector<std::string> columns) && {
  return GroupedQueryBuilder(std::move(*this), std::move(columns));
}

}  // namespace quest
