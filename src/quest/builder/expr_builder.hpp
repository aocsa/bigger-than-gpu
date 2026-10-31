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

#include "quest/logical_plan/logical_expr.hpp"

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
  explicit ExprBuilder(LogicalExprPtr expr) noexcept : expr_(std::move(expr)) {}

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

  [[nodiscard]] LogicalExprPtr Build() const { return expr_->Clone(); }

  [[nodiscard]] const LogicalExpr& GetExpr() const noexcept { return *expr_; }

  /// @brief Check if the expression is valid (non-null).
  [[nodiscard]] explicit operator bool() const noexcept { return expr_ != nullptr; }

 private:
  LogicalExprPtr expr_;
};

// =============================================================================
// Expression Factory Functions
// =============================================================================

[[nodiscard]] inline ExprBuilder Col(const std::string& name) {
  return ExprBuilder(col(name));
}

template <LiteralValue T>
[[nodiscard]] ExprBuilder Lit(T value) {
  return ExprBuilder(lit(std::move(value)));
}

}  // namespace quest
