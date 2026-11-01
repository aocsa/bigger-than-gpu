// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file expr_builder.hpp
/// @brief Expression builder DSL for constructing logical expressions.
///
/// Provides a fluent interface for building filter and projection expressions:
/// @code
///   auto expr = Col("salary") > Lit(50000) && Col("department") == Lit("Engineering");
/// @endcode

#include <concepts>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "quest/plan/logical/logical_expr.hpp"

namespace quest {

// =============================================================================
// Concepts for Type Safety
// =============================================================================

/// @brief Concept for types that can be used as literal values in expressions.
template <typename T>
concept LiteralValue = std::is_arithmetic_v<T> || std::same_as<T, std::string> ||
                       std::same_as<T, std::string_view> || std::same_as<T, bool>;

// =============================================================================
// Expression Builder
// =============================================================================

/// @brief Expression builder for fluent expression construction.
///
/// Provides operator overloads for intuitive expression building:
/// @code
///   auto expr = Col("salary") > Lit(50000) && Col("department") == Lit("Engineering");
/// @endcode
class ExprBuilder {
 public:
  /// @brief Construct from a logical expression (takes ownership).
  explicit ExprBuilder(plan::logical::LogicalExprPtr expr) noexcept : expr_(std::move(expr)) {}

  // Rule of Five: Default move, deep-clone copy
  ExprBuilder(ExprBuilder&&) noexcept = default;
  ExprBuilder& operator=(ExprBuilder&&) noexcept = default;
  ExprBuilder(const ExprBuilder& other) : expr_(other.expr_->Clone()) {}
  ExprBuilder& operator=(const ExprBuilder& other) {
    if (this != &other) {
      expr_ = other.expr_->Clone();
    }
    return *this;
  }
  ~ExprBuilder() = default;

  // ===========================================================================
  // Comparison Operators
  // ===========================================================================

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

  // ===========================================================================
  // Logical Operators
  // ===========================================================================

  [[nodiscard]] ExprBuilder operator&&(const ExprBuilder& other) const {
    return ExprBuilder(and_expr(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator||(const ExprBuilder& other) const {
    return ExprBuilder(or_expr(expr_->Clone(), other.expr_->Clone()));
  }

  // ===========================================================================
  // Arithmetic Operators
  // ===========================================================================

  [[nodiscard]] ExprBuilder operator+(const ExprBuilder& other) const {
    return ExprBuilder(add(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator-(const ExprBuilder& other) const {
    return ExprBuilder(subtract(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator*(const ExprBuilder& other) const {
    return ExprBuilder(multiply(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator/(const ExprBuilder& other) const {
    return ExprBuilder(divide(expr_->Clone(), other.expr_->Clone()));
  }

  [[nodiscard]] ExprBuilder operator%(const ExprBuilder& other) const {
    return ExprBuilder(modulo(expr_->Clone(), other.expr_->Clone()));
  }

  // ===========================================================================
  // Build Methods
  // ===========================================================================

  [[nodiscard]] plan::logical::LogicalExprPtr Build() const { return expr_->Clone(); }

  [[nodiscard]] const plan::logical::LogicalExpr& GetExpr() const noexcept { return *expr_; }

  /// @brief Check if the expression is valid (non-null).
  [[nodiscard]] explicit operator bool() const noexcept { return expr_ != nullptr; }

 private:
  plan::logical::LogicalExprPtr expr_;
};

// =============================================================================
// Expression Factory Functions
// =============================================================================

[[nodiscard]] inline ExprBuilder Col(const std::string& name) {
  return ExprBuilder(plan::logical::col(name));
}

template <LiteralValue T>
[[nodiscard]] ExprBuilder Lit(T value) {
  return ExprBuilder(plan::logical::lit(std::move(value)));
}

/// @brief `CASE WHEN when THEN then ELSE otherwise END`.
/// @param when Bool condition; null counts as not true.
/// @param then Value for rows where @p when is true.
/// @param otherwise Value for the other rows.
/// @return The CASE expression.
[[nodiscard]] inline ExprBuilder Case(const ExprBuilder& when, const ExprBuilder& then,
                                      const ExprBuilder& otherwise) {
  return ExprBuilder(plan::logical::case_when(when.Build(), then.Build(), otherwise.Build()));
}

/// @brief Aggregate functions for `GroupBy(...).Aggregate(...)`. A namespace,
///        because the logical plan already has classes named Sum, Count, ...
namespace agg {

[[nodiscard]] inline ExprBuilder Sum(const ExprBuilder& input) {
  return ExprBuilder(plan::logical::sum(input.Build()));
}
[[nodiscard]] inline ExprBuilder Avg(const ExprBuilder& input) {
  return ExprBuilder(plan::logical::avg(input.Build()));
}
[[nodiscard]] inline ExprBuilder Count(const ExprBuilder& input) {
  return ExprBuilder(plan::logical::count(input.Build()));
}
[[nodiscard]] inline ExprBuilder CountStar() { return ExprBuilder(plan::logical::count_star()); }
[[nodiscard]] inline ExprBuilder Min(const ExprBuilder& input) {
  return ExprBuilder(plan::logical::min(input.Build()));
}
[[nodiscard]] inline ExprBuilder Max(const ExprBuilder& input) {
  return ExprBuilder(plan::logical::max(input.Build()));
}

}  // namespace agg

}  // namespace quest
