// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file binary_op.hpp
/// @brief Binary operator kinds shared by the logical and physical plan layers.

#include <algorithm>
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

/// @brief @p type as a Decimal: an integer is Decimal(digits, 0), a Decimal is itself.
[[nodiscard]] constexpr columnar::DataType AsDecimal(columnar::DataType type) noexcept {
  using columnar::TypeId;
  switch (type.id) {
    case TypeId::kInt8:
      return columnar::DataType::Decimal(3, 0);
    case TypeId::kInt16:
      return columnar::DataType::Decimal(5, 0);
    case TypeId::kInt32:
      return columnar::DataType::Decimal(10, 0);
    case TypeId::kInt64:
      return columnar::DataType::Decimal(columnar::DataType::kMaxDecimalPrecision, 0);
    default:
      return type;
  }
}

/// @brief SQL result type of `left op right`, shared by `BinaryExpr::ToField` and
/// `BinaryExpression::Evaluate` so both layers agree.
///
/// Comparisons and AND/OR yield bool. Arithmetic needs numeric operands, typed as DuckDB
/// types them: any float or double operand, or `/`, yields double (int / int is real
/// division); two integers yield the wider integer. With a Decimal operand (an integer counts
/// as Decimal(digits, 0)), `+` and `-` keep the larger scale, `*` adds the scales, and `%`
/// yields double. A Decimal has at most 18 digits; the kernels fail on overflow. Callers
/// resolve a NULL literal operand, which has no type of its own, to the other operand's
/// type. A Date32 or Timestamp compares only with the same type: there is no implicit cast
/// to or from a temporal type.
/// @param op Operator.
/// @param left Left operand type.
/// @param right Right operand type.
/// @return The result type, or `TypeError` for arithmetic on a non-numeric type, a product
/// with more than 18 decimals, or a comparison between a temporal type and a different type.
[[nodiscard]] inline Result<columnar::DataType> BinaryResultType(BinaryOp op,
                                                                 columnar::DataType left,
                                                                 columnar::DataType right) {
  using columnar::DataType;
  if (!IsArithmetic(op)) {
    if ((columnar::IsTemporal(left) || columnar::IsTemporal(right)) && left != right) {
      return Error::TypeError("'" + std::string(ToString(op)) + "' cannot compare " +
                              columnar::GetTypeName(left) + " with " +
                              columnar::GetTypeName(right));
    }
    return DataType::kBool;
  }
  if (!columnar::IsNumeric(left) || !columnar::IsNumeric(right)) {
    return Error::TypeError("'" + std::string(ToString(op)) + "' needs numeric operands, got " +
                            columnar::GetTypeName(left) + " and " +
                            columnar::GetTypeName(right));
  }
  const bool exact = (columnar::IsInteger(left) || columnar::IsDecimal(left)) &&
                     (columnar::IsInteger(right) || columnar::IsDecimal(right));
  const bool decimal = columnar::IsDecimal(left) || columnar::IsDecimal(right);
  if (!exact || op == BinaryOp::kDivide || (decimal && op == BinaryOp::kModulo)) {
    return DataType::kDouble;
  }
  if (!decimal) {
    // kInt8..kInt64 are declared in width order.
    return left.id >= right.id ? left : right;
  }
  const DataType l = AsDecimal(left);
  const DataType r = AsDecimal(right);
  constexpr int kMax = DataType::kMaxDecimalPrecision;
  if (op == BinaryOp::kMultiply) {
    const int scale = l.scale + r.scale;
    if (scale > kMax) {
      return Error::TypeError("'*' of " + columnar::GetTypeName(left) + " and " +
                              columnar::GetTypeName(right) + " has more than 18 decimals");
    }
    const int precision = std::max(scale, std::min(kMax, l.precision + r.precision));
    return DataType::Decimal(static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale));
  }
  const int scale = std::max(l.scale, r.scale);
  const int integer_digits = std::max(l.precision - l.scale, r.precision - r.scale);
  const int precision = std::min(kMax, integer_digits + scale + 1);
  return DataType::Decimal(static_cast<std::uint8_t>(precision), static_cast<std::uint8_t>(scale));
}

/// @brief Type that both @p left and @p right convert to, e.g. the result type of the
/// branches of a CASE; shared by `CaseExpr::ToField` and `CaseExpression::Evaluate`.
///
/// Equal types yield that type; two numeric types widen as `BinaryResultType` does for `+`.
/// Callers resolve a NULL literal, which has no type of its own, to the other type.
/// @param left First type.
/// @param right Second type.
/// @return The common type, or `TypeError` if the types are incompatible.
[[nodiscard]] inline Result<columnar::DataType> CommonType(columnar::DataType left,
                                                           columnar::DataType right) {
  if (left == right) {
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
