// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_QUERY_BUILDER_HPP
#define QUEST_QUERY_BUILDER_HPP

/// @file query_builder.hpp
/// @brief Fluent query builder DSL for constructing query plans.
///
/// This file provides a fluent interface for building query plans,
/// similar to DataFrame APIs in other data processing frameworks.

#include <functional>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/datasource/data_source.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/physical_plan.hpp"
#include "quest/result.hpp"

namespace quest {

// =============================================================================
// Expression Builder
// =============================================================================

/// @brief Expression builder for fluent expression construction.
class ExprBuilder {
 public:
  explicit ExprBuilder(LogicalExprPtr expr) : expr_(std::move(expr)) {}

  // Comparison operators
  [[nodiscard]] ExprBuilder operator==(const ExprBuilder& other) const {
    return ExprBuilder(eq(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator!=(const ExprBuilder& other) const {
    return ExprBuilder(neq(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator<(const ExprBuilder& other) const {
    return ExprBuilder(lt(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator<=(const ExprBuilder& other) const {
    return ExprBuilder(lte(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator>(const ExprBuilder& other) const {
    return ExprBuilder(gt(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator>=(const ExprBuilder& other) const {
    return ExprBuilder(gte(expr_->Clone(), other.expr_->Clone()));
  }

  // Logical operators
  [[nodiscard]] ExprBuilder operator&&(const ExprBuilder& other) const {
    return ExprBuilder(and_expr(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator||(const ExprBuilder& other) const {
    return ExprBuilder(or_expr(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] LogicalExprPtr Build() const { return expr_->Clone(); }

  [[nodiscard]] const LogicalExpr& GetExpr() const { return *expr_; }

 private:
  LogicalExprPtr expr_;
};

/// @brief Column reference for DSL.
[[nodiscard]] inline ExprBuilder Col(const std::string& name) {
  return ExprBuilder(col(name));
}

/// @brief Literal value for DSL.
template <typename T>
[[nodiscard]] ExprBuilder Lit(T value) {
  return ExprBuilder(lit(value));
}

// =============================================================================
// Query Builder
// =============================================================================

/// @brief Query builder for fluent query construction.
class QueryBuilder {
 public:
  /// @brief Start with a Parquet data source.
  /// @param filename Path to the Parquet file.
  /// @return Result containing QueryBuilder or error.
  [[nodiscard]] static Result<QueryBuilder> FromParquet(
      const std::string& filename) {
    auto source_result = datasource::ParquetDataSource::Open(filename);
    if (!source_result.has_value()) {
      return std::unexpected(source_result.error());
    }
    // Move the ParquetDataSource into a shared_ptr
    auto source = std::make_shared<datasource::ParquetDataSource>(
        std::move(source_result.value()));
    return QueryBuilder(
        std::make_unique<Scan>(filename, std::move(source), std::nullopt));
  }

  /// @brief Start with a generic data source.
  /// @param source The data source.
  /// @param name A name for the data source.
  /// @return QueryBuilder instance.
  [[nodiscard]] static QueryBuilder From(std::shared_ptr<DataSource> source,
                                         const std::string& name) {
    return QueryBuilder(
        std::make_unique<Scan>(name, std::move(source), std::nullopt));
  }

  /// @brief Add a filter/selection.
  [[nodiscard]] QueryBuilder Where(const ExprBuilder& condition) && {
    return QueryBuilder(
        std::make_unique<Selection>(std::move(plan_), condition.Build()));
  }

  /// @brief Add a projection with column names.
  [[nodiscard]] QueryBuilder Select(
      std::initializer_list<std::string> columns) && {
    std::vector<LogicalExprPtr> exprs;
    exprs.reserve(columns.size());
    for (const auto& name : columns) {
      exprs.push_back(col(name));
    }
    return QueryBuilder(
        std::make_unique<Projection>(std::move(plan_), std::move(exprs)));
  }

  /// @brief Add a projection with expressions.
  [[nodiscard]] QueryBuilder Select(
      std::initializer_list<ExprBuilder> exprs) && {
    std::vector<LogicalExprPtr> expr_list;
    expr_list.reserve(exprs.size());
    for (const auto& expr : exprs) {
      expr_list.push_back(expr.Build());
    }
    return QueryBuilder(
        std::make_unique<Projection>(std::move(plan_), std::move(expr_list)));
  }

  /// @brief Build and return the logical plan.
  [[nodiscard]] LogicalPlanPtr BuildLogicalPlan() && { return std::move(plan_); }

  /// @brief Build and return the physical plan.
  [[nodiscard]] Result<PhysicalPlanPtr> BuildPhysicalPlan() && {
    return CreatePhysicalPlan(*plan_);
  }

  /// @brief Execute the query and call callback for each batch.
  [[nodiscard]] Status Execute(const ResultCallback& callback) && {
    auto physical_result = CreatePhysicalPlan(*plan_);
    if (!physical_result.has_value()) {
      return physical_result.error();
    }
    return physical_result.value()->Execute(callback);
  }

  /// @brief Execute and collect all results.
  [[nodiscard]] Result<std::vector<RecordBatch>> ExecuteCollect() && {
    std::vector<RecordBatch> results;
    auto physical_result = CreatePhysicalPlan(*plan_);
    if (!physical_result.has_value()) {
      return std::unexpected(physical_result.error());
    }
    auto status = physical_result.value()->Execute(
        [&results](RecordBatch batch) { results.push_back(std::move(batch)); });
    if (!status.ok()) {
      return std::unexpected(status);
    }
    return results;
  }

  /// @brief Get string representation of the logical plan.
  [[nodiscard]] std::string ToString() const { return plan_->Pretty(); }

 private:
  explicit QueryBuilder(LogicalPlanPtr plan) : plan_(std::move(plan)) {}

  LogicalPlanPtr plan_;
};

/// @brief Macro-based DSL for query definition.
///
/// Usage:
/// @code
///   QUEST_QUERY(my_query) {
///     auto result = QueryBuilder::FromParquet("file.parquet");
///     if (!result.has_value()) { /* handle error */ }
///     return std::move(result.value())
///         .Where(Col("salary") == Lit(200))
///         .Select({"id", "name"});
///   }
/// @endcode
#define QUEST_QUERY(name) inline auto name() -> quest::QueryBuilder

}  // namespace quest

#endif  // QUEST_QUERY_BUILDER_HPP
