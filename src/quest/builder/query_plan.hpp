// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file query_plan.hpp
/// @brief Query plan building logic behind QueryBuilder's fluent API.

#include <cstddef>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/common/result.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/io/parquet/parquet_data_source.hpp"
#include "quest/plan/logical/logical_plan.hpp"

namespace quest {

// =============================================================================
// Query Plan
// =============================================================================

/// @brief Shared query plan that holds the logical plan and provides plan-building methods.
///
/// QueryBuilder composes this class for the fluent API. QueryPlan owns the
/// logical plan and provides factory methods, filtering, projection, and plan extraction.
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

  [[nodiscard]] static QueryPlan From(std::shared_ptr<DataSource> source,
                                      const std::string& name) {
    return QueryPlan(
        std::make_unique<Scan>(name, std::move(source), std::nullopt));
  }

  // ===========================================================================
  // Query Building (Fluent Interface)
  // ===========================================================================

  [[nodiscard]] QueryPlan Where(const ExprBuilder& condition) && {
    return QueryPlan(
        std::make_unique<Selection>(std::move(plan_), condition.Build()));
  }

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

  [[nodiscard]] QueryPlan Select(const std::vector<ExprBuilder>& exprs) && {
    std::vector<LogicalExprPtr> expr_list;
    expr_list.reserve(exprs.size());
    for (const auto& expr : exprs) {
      expr_list.push_back(expr.Build());
    }
    return QueryPlan(
        std::make_unique<Projection>(std::move(plan_), std::move(expr_list)));
  }

  [[nodiscard]] QueryPlan Limit(std::size_t n) && {
    return QueryPlan(std::make_unique<quest::Limit>(std::move(plan_), n));
  }

  /// @brief Concatenate this plan with @p other (UNION ALL). This plan is the
  ///        left input, @p other the right; both must share a schema.
  [[nodiscard]] QueryPlan UnionAll(QueryPlan other) && {
    return QueryPlan(std::make_unique<quest::Union>(std::move(plan_), std::move(other.plan_)));
  }

  // ===========================================================================
  // Plan Building
  // ===========================================================================

  [[nodiscard]] LogicalPlanPtr BuildLogicalPlan() && { return std::move(plan_); }

  // ===========================================================================
  // Utility Methods
  // ===========================================================================

  [[nodiscard]] std::string ToString() const { return plan_->Pretty(); }

  [[nodiscard]] explicit operator bool() const noexcept { return plan_ != nullptr; }

  [[nodiscard]] datasource::Schema schema() const { return plan_->schema(); }

 private:
  template <typename>
  friend class QueryBuilderOf;
  friend class QueryBuilder;

  explicit QueryPlan(LogicalPlanPtr plan) noexcept : plan_(std::move(plan)) {}

  /// @brief Get a const reference to the logical plan (for execution by builders).
  [[nodiscard]] const LogicalPlan& plan() const { return *plan_; }

  LogicalPlanPtr plan_;
};

}  // namespace quest
