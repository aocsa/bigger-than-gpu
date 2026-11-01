// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file binary_op.hpp
/// @brief Binary operator kinds shared by the logical and physical plan layers.

#include <array>
#include <string>
#include <string_view>
#include <utility>

#include "quest/columnar/types.hpp"
#include "quest/common/result.hpp"

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

/// @brief SQL result type of `left op right`, shared by `BinaryExpr::ToField` and
/// `BinaryExpression::Evaluate` so both layers agree.
///
/// Comparisons and AND/OR yield bool. Arithmetic needs numeric operands: any float or
/// double operand, or `/`, yields double (int / int is real division, as in DuckDB);
/// otherwise the wider of the two integer types. Callers resolve a NULL literal operand,
/// which has no type of its own, to the other operand's type.
/// @param op Operator.
/// @param left Left operand type.
/// @param right Right operand type.
/// @return The result type, or `TypeError` for arithmetic on a non-numeric type.
[[nodiscard]] inline Result<columnar::DataType> BinaryResultType(BinaryOp op,
                                                                 const columnar::DataType& left,
                                                                 const columnar::DataType& right) {
  if (!IsArithmetic(op)) {
    return columnar::BoolType{};
  }
  auto is_int = [](const columnar::DataType& t) {
    return std::holds_alternative<columnar::Int8Type>(t) ||
           std::holds_alternative<columnar::Int16Type>(t) ||
           std::holds_alternative<columnar::Int32Type>(t) ||
           std::holds_alternative<columnar::Int64Type>(t);
  };
  auto is_float = [](const columnar::DataType& t) {
    return std::holds_alternative<columnar::FloatType>(t) ||
           std::holds_alternative<columnar::DoubleType>(t);
  };
  if (!(is_int(left) || is_float(left)) || !(is_int(right) || is_float(right))) {
    return Error::TypeError("'" + std::string(ToString(op)) + "' needs numeric operands, got " +
                            columnar::GetTypeName(left) + " and " +
                            columnar::GetTypeName(right));
  }
  if (is_float(left) || is_float(right) || op == BinaryOp::kDivide) {
    return columnar::DoubleType{};
  }
  // Int8Type..Int64Type are declared in width order in DataType.
  return left.index() >= right.index() ? left : right;
}

/// @brief Type that both @p left and @p right convert to, e.g. the result type of the
/// branches of a CASE; shared by `CaseExpr::ToField` and `CaseExpression::Evaluate`.
///
/// Equal types yield that type; two numeric types widen as `BinaryResultType` does for `+`.
/// Callers resolve a NULL literal, which has no type of its own, to the other type.
/// @param left First type.
/// @param right Second type.
/// @return The common type, or `TypeError` if the types are incompatible.
[[nodiscard]] inline Result<columnar::DataType> CommonType(const columnar::DataType& left,
                                                           const columnar::DataType& right) {
  if (left.index() == right.index()) {
    return left;
  }
  auto widened = BinaryResultType(BinaryOp::kAdd, left, right);
  if (!widened) {
    return Error::TypeError("No common type for " + columnar::GetTypeName(left) + " and " +
                            columnar::GetTypeName(right));
  }
  return widened;
}

}  // namespace quest::plan
