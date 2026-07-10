// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_QUERY_PLAN_HPP
#define QUEST_QUERY_PLAN_HPP

/// @file query_plan.hpp
/// @brief Shared query plan building logic used by both sync and streaming query builders.

#include <concepts>
#include <cstddef>
#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/physical_plan.hpp"
#include "quest/result.hpp"

namespace quest {

// =============================================================================
// Execution Concepts
// =============================================================================

/// @brief Concept for callable types that accept RecordBatch.
template <typename F>
concept BatchCallback = std::invocable<F, datasource::RecordBatch> &&
                        std::same_as<std::invoke_result_t<F, datasource::RecordBatch>, void>;

/// @brief Concept for callable types that accept RecordBatch and return control flow.
template <typename F>
concept BatchCallbackWithControl =
    std::invocable<F, datasource::RecordBatch> &&
    std::convertible_to<std::invoke_result_t<F, datasource::RecordBatch>, bool>;

// =============================================================================
// Query Plan
// =============================================================================

/// @brief Shared query plan that holds the logical plan and provides plan-building methods.
///
/// Both QueryBuilder and StreamingQueryBuilder compose this class to share the
/// fluent API for constructing queries. QueryPlan owns the logical plan and
/// provides factory methods, filtering, projection, and plan extraction.
class QueryPlan {
 public:
  // Rule of Five: Move-only (plan ownership)
  QueryPlan(QueryPlan&&) noexcept = default;
  QueryPlan& operator=(QueryPlan&&) noexcept = default;
  QueryPlan(const QueryPlan&) = delete;
  QueryPlan& operator=(const QueryPlan&) = delete;
  ~QueryPlan() = default;

  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  /// @brief Start with a Parquet data source.
  /// @param filename Path to the Parquet file.
  /// @return Result containing QueryPlan or error.
  [[nodiscard]] static Result<QueryPlan> FromParquet(
      const std::string& filename) {
    auto source_result = datasource::ParquetDataSource::Open(filename);
    if (!source_result) {
      return std::unexpected(source_result.error());
    }
    auto source = std::make_shared<datasource::ParquetDataSource>(
        std::move(*source_result));
    return QueryPlan(
        std::make_unique<Scan>(filename, std::move(source), std::nullopt));
  }

  /// @brief Start with a generic data source.
  /// @param source The data source (shared ownership).
  /// @param name A name for the data source.
  /// @return QueryPlan instance.
  [[nodiscard]] static QueryPlan From(std::shared_ptr<DataSource> source,
                                      const std::string& name) {
    return QueryPlan(
        std::make_unique<Scan>(name, std::move(source), std::nullopt));
  }

  // ===========================================================================
  // Query Building (Fluent Interface)
  // ===========================================================================

  /// @brief Add a filter/selection condition.
  /// @param condition The filter expression.
  /// @return QueryPlan with the filter applied.
  [[nodiscard]] QueryPlan Where(const ExprBuilder& condition) && {
    return QueryPlan(
        std::make_unique<Selection>(std::move(plan_), condition.Build()));
  }

  /// @brief Add a projection with column names.
  /// @param columns List of column names to select.
  /// @return QueryPlan with the projection applied.
  [[nodiscard]] QueryPlan Select(
      std::initializer_list<std::string> columns) && {
    std::vector<LogicalExprPtr> exprs;
    exprs.reserve(columns.size());
    for (const auto& name : columns) {
      exprs.push_back(col(name));
    }
    return QueryPlan(
        std::make_unique<Projection>(std::move(plan_), std::move(exprs)));
  }

  /// @brief Add a projection with expressions.
  /// @param exprs List of expressions to compute.
  /// @return QueryPlan with the projection applied.
  [[nodiscard]] QueryPlan Select(
      std::initializer_list<ExprBuilder> exprs) && {
    std::vector<LogicalExprPtr> expr_list;
    expr_list.reserve(exprs.size());
    for (const auto& expr : exprs) {
      expr_list.push_back(expr.Build());
    }
    return QueryPlan(
        std::make_unique<Projection>(std::move(plan_), std::move(expr_list)));
  }

  /// @brief Add a projection with a vector of expressions.
  /// @param exprs Vector of expressions to compute.
  /// @return QueryPlan with the projection applied.
  [[nodiscard]] QueryPlan Select(const std::vector<ExprBuilder>& exprs) && {
    std::vector<LogicalExprPtr> expr_list;
    expr_list.reserve(exprs.size());
    for (const auto& expr : exprs) {
      expr_list.push_back(expr.Build());
    }
    return QueryPlan(
        std::make_unique<Projection>(std::move(plan_), std::move(expr_list)));
  }

  /// @brief Truncate the result to the first @p n rows.
  /// @param n Maximum number of rows to keep.
  /// @return QueryPlan with the limit applied.
  [[nodiscard]] QueryPlan Limit(std::size_t n) && {
    return QueryPlan(std::make_unique<quest::Limit>(std::move(plan_), n));
  }

  // ===========================================================================
  // Plan Building
  // ===========================================================================

  /// @brief Build and return the logical plan.
  [[nodiscard]] LogicalPlanPtr BuildLogicalPlan() && { return std::move(plan_); }

  /// @brief Build and return the physical plan.
  [[nodiscard]] Result<PhysicalPlanPtr> BuildPhysicalPlan() && {
    return CreatePhysicalPlan(*plan_);
  }

  // ===========================================================================
  // Utility Methods
  // ===========================================================================

  /// @brief Get string representation of the logical plan.
  [[nodiscard]] std::string ToString() const { return plan_->Pretty(); }

  /// @brief Check if the plan is valid (non-null).
  [[nodiscard]] explicit operator bool() const noexcept { return plan_ != nullptr; }

  /// @brief Get the output schema of the query.
  [[nodiscard]] datasource::Schema schema() const { return plan_->schema(); }

 private:
  template <typename>
  friend class QueryBuilderOf;
  friend class QueryBuilder;
  friend class StreamingQueryBuilder;

  explicit QueryPlan(LogicalPlanPtr plan) noexcept : plan_(std::move(plan)) {}

  /// @brief Get a const reference to the logical plan (for execution by builders).
  [[nodiscard]] const LogicalPlan& plan() const { return *plan_; }

  LogicalPlanPtr plan_;
};

}  // namespace quest

#endif  // QUEST_QUERY_PLAN_HPP
