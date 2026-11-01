// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file binary_op.hpp
/// @brief Binary operator kinds shared by the logical and physical plan layers.

#include <array>
#include <string_view>
#include <utility>

namespace quest::plan {

/// @brief Binary operator of a `BinaryExpr` / `BinaryExpression`.
///
/// Arithmetic operators come last; `IsArithmetic` relies on that order.
enum class BinaryOp {
  kEq,
  kNeq,
  kLt,
  kLtEq,
  kGt,
  kGtEq,
  kAnd,
  kOr,
  kAdd,
  kSubtract,
  kMultiply,
  kDivide,
  kModulo,
};

inline constexpr std::array<std::string_view, 13> kBinaryOpSymbols = {
    "==", "!=", "<", "<=", ">", ">=", "AND", "OR", "+", "-", "*", "/", "%"};

/// @brief Symbol printed between the operands, e.g. `"<="` or `"AND"`.
/// @param op Operator to name.
/// @return The operator's symbol.
[[nodiscard]] constexpr std::string_view ToString(BinaryOp op) noexcept {
  return kBinaryOpSymbols[std::to_underlying(op)];
}

/// @brief Whether @p op yields a number (true) or a boolean (false).
/// @param op Operator to classify.
/// @return True for `+ - * / %`.
[[nodiscard]] constexpr bool IsArithmetic(BinaryOp op) noexcept {
  return std::to_underlying(op) >= std::to_underlying(BinaryOp::kAdd);
}

}  // namespace quest::plan
