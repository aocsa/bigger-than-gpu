// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_QUERY_BUILDER_HPP
#define QUEST_QUERY_BUILDER_HPP

/// @file query_builder.hpp
/// @brief Synchronous query builder with push-based execution.
///
/// Provides three execution modes:
/// 1. **Callback**: Execute() - push-based, caller provides callback
/// 2. **Early termination**: ExecuteWithControl() - callback returns bool to stop
/// 3. **Collect all**: ExecuteCollect() - collects all results into memory
///
/// For pull-based streaming with backpressure, see streaming_query_builder.hpp.

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/builder/query_plan.hpp"
#include "quest/execution/graph_builder.hpp"
#include "quest/memory/memory_resource.hpp"

namespace quest {

// =============================================================================
// Query Builder (Synchronous Execution)
// =============================================================================

/// @brief Synchronous query builder for push-based execution.
///
/// Example:
/// @code
///   auto result = QueryBuilder::FromParquet("file.parquet");
///   if (!result) { /* handle error */ }
///
///   auto batches = std::move(result.value())
///       .Where(Col("salary") > Lit(50000))
///       .Select({"id", "name", "salary"})
///       .ExecuteCollect();
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
  [[nodiscard]] static QueryBuilder From(std::shared_ptr<DataSource> source,
                                         const std::string& name) {
    return QueryBuilder(QueryPlan::From(std::move(source), name));
  }

  // ===========================================================================
  // Query Building (Fluent Interface)
  // ===========================================================================

  /// @brief Add a filter/selection condition.
  [[nodiscard]] QueryBuilder Where(const ExprBuilder& condition) && {
    return QueryBuilder(std::move(plan_).Where(condition));
  }

  /// @brief Add a projection with column names.
  [[nodiscard]] QueryBuilder Select(std::initializer_list<std::string> columns) && {
    return QueryBuilder(std::move(plan_).Select(columns));
  }

  /// @brief Add a projection with expressions.
  [[nodiscard]] QueryBuilder Select(std::initializer_list<ExprBuilder> exprs) && {
    return QueryBuilder(std::move(plan_).Select(exprs));
  }

  /// @brief Add a projection with a vector of expressions.
  [[nodiscard]] QueryBuilder Select(const std::vector<ExprBuilder>& exprs) && {
    return QueryBuilder(std::move(plan_).Select(exprs));
  }

  /// @brief Truncate the result to the first @p n rows.
  [[nodiscard]] QueryBuilder Limit(std::size_t n) && {
    return QueryBuilder(std::move(plan_).Limit(n));
  }

  // ===========================================================================
  // Plan Building
  // ===========================================================================

  /// @brief Build and return the logical plan.
  [[nodiscard]] LogicalPlanPtr BuildLogicalPlan() && { return std::move(plan_).BuildLogicalPlan(); }

  /// @brief Build and return the physical plan.
  [[nodiscard]] Result<PhysicalPlanPtr> BuildPhysicalPlan() && {
    return std::move(plan_).BuildPhysicalPlan();
  }

  // ===========================================================================
  // Execution Methods (Synchronous / Push-based)
  // ===========================================================================

  /// @brief Execute the query with a callback for each batch (push-based).
  /// @tparam F Callback type (must satisfy BatchCallback concept).
  /// @param callback Function called for each result batch.
  /// @return Status indicating success or failure.
  template <BatchCallback F>
  [[nodiscard]] Status Execute(F&& callback) && {
    auto physical_result = CreatePhysicalPlan(plan_.plan());
    if (!physical_result) {
      return std::unexpected(physical_result.error());
    }
    return physical_result.value()->Execute(
        [&callback](datasource::RecordBatch batch) {
          std::forward<F>(callback)(std::move(batch));
          return true;
        });
  }

  /// @brief Execute with early termination support (callback returns bool).
  /// @tparam F Callback type (must satisfy BatchCallbackWithControl concept).
  /// @param callback Function returning false to stop execution.
  /// @return Status indicating success, failure, or cancellation.
  template <BatchCallbackWithControl F>
  [[nodiscard]] Status ExecuteWithControl(F&& callback) && {
    auto physical_result = CreatePhysicalPlan(plan_.plan());
    if (!physical_result) {
      return std::unexpected(physical_result.error());
    }

    bool cancelled = false;
    auto status = physical_result.value()->Execute(
        [&callback, &cancelled](datasource::RecordBatch batch) {
          if (!std::forward<F>(callback)(std::move(batch))) {
            cancelled = true;
            return false;  // Stop Scan — do not drain remaining batches.
          }
          return true;
        });

    if (cancelled) {
      return Status::Cancelled("Query execution cancelled by callback");
    }
    return status;
  }

  /// @brief Execute and collect all results into memory.
  /// @return Vector of RecordBatches or error.
  /// @note For large datasets, consider StreamingQueryBuilder::ExecuteStream().
  [[nodiscard]] Result<std::vector<datasource::RecordBatch>> ExecuteCollect() && {
    std::vector<datasource::RecordBatch> results;
    auto physical_result = CreatePhysicalPlan(plan_.plan());
    if (!physical_result) {
      return std::unexpected(physical_result.error());
    }
    auto status = physical_result.value()->Execute(
        [&results](datasource::RecordBatch batch) {
          results.push_back(std::move(batch));
          return true;
        });
    if (!status) {
      return std::unexpected(status.error());
    }
    return results;
  }

  /// @brief Execute via the push-pipeline Graph engine (Phase 1 bridge).
  ///
  /// Tries `CreateGraph` → `Graph::Execute()` (PipelineExecutor). On an
  /// unsupported logical node, falls back to the callback physical plan —
  /// Sirius create_plan → CPU fallback discipline at CPU scale.
  ///
  /// Default `Execute` / `ExecuteCollect` are unchanged.
  ///
  /// @param memory_manager Optional memory manager (spill budget).
  /// @return Collected RecordBatches, or error.
  [[nodiscard]] Result<std::vector<datasource::RecordBatch>> ExecuteOnGraph(
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr) && {
    auto graph_result = execution::CreateGraph(plan_.plan(), std::move(memory_manager));
    if (graph_result) {
      auto exec = std::move(*graph_result);
      auto status = exec->Execute();
      if (!status) {
        return std::unexpected(status.error());
      }
      return exec->collector().CollectBatches();
    }

    // Unsupported plan → callback engine (executor of last resort).
    std::vector<datasource::RecordBatch> results;
    auto physical_result = CreatePhysicalPlan(plan_.plan());
    if (!physical_result) {
      // Prefer the CreateGraph error (unsupported node) when both fail.
      return std::unexpected(graph_result.error());
    }
    auto status = physical_result.value()->Execute(
        [&results](datasource::RecordBatch batch) {
          results.push_back(std::move(batch));
          return true;
        });
    if (!status) {
      return std::unexpected(status.error());
    }
    return results;
  }

  // ===========================================================================
  // Utility Methods
  // ===========================================================================

  /// @brief Get string representation of the logical plan.
  [[nodiscard]] std::string ToString() const { return plan_.ToString(); }

  /// @brief Check if the builder has a valid plan.
  [[nodiscard]] explicit operator bool() const noexcept { return static_cast<bool>(plan_); }

  /// @brief Get the output schema of the query.
  [[nodiscard]] datasource::Schema schema() const { return plan_.schema(); }

 private:
  explicit QueryBuilder(QueryPlan plan) noexcept : plan_(std::move(plan)) {}

  QueryPlan plan_;
};

}  // namespace quest

#endif  // QUEST_QUERY_BUILDER_HPP
