// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/physical/expressions/physical_expr.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <optional>
#include <string>

namespace quest::plan::physical {

namespace {

/// @brief Compares two values; null if either is null. Mixed numeric types compare as
/// double; other mismatched types compare false.
template <typename Op>
std::optional<bool> CompareValues(const Value& left, const Value& right) {
  return std::visit(
      [](const auto& lhs, const auto& rhs) -> std::optional<bool> {
        using LT = std::decay_t<decltype(lhs)>;
        using RT = std::decay_t<decltype(rhs)>;

        if constexpr (std::is_same_v<LT, std::monostate> ||
                      std::is_same_v<RT, std::monostate>) {
          return std::nullopt;
        } else if constexpr (std::is_same_v<LT, RT>) {
          return Op{}(lhs, rhs);
        } else if constexpr (std::is_arithmetic_v<LT> &&
                             std::is_arithmetic_v<RT>) {
          return Op{}(static_cast<double>(lhs), static_cast<double>(rhs));
        } else {
          return false;
        }
      },
      left, right);
}

/// @brief A one-row column holding @p value.
template <typename T>
[[nodiscard]] ColumnVectorPtr OneRow(DataType type, T value) {
  columnar::ColumnBuilder<T> builder(type, 1);
  builder.Append(value);
  return std::move(builder).Finish();
}

/// @brief Applies @p fn to each row pair; a `std::nullopt` row result is stored as null.
template <typename T, typename Fn>
[[nodiscard]] Result<ColumnVectorPtr> MapRows(const ColumnVector& left, const ColumnVector& right,
                                              size_t num_rows, DataType type, Fn fn) {
  columnar::ColumnBuilder<T> builder(type, num_rows);
  for (size_t i = 0; i < num_rows; ++i) {
    Result<std::optional<T>> row = fn(left.GetValue(i), right.GetValue(i));
    if (!row) {
      return std::unexpected(row.error());
    }
    *row ? builder.Append(**row) : builder.AppendNull();
  }
  return std::move(builder).Finish();
}

[[nodiscard]] Result<std::optional<bool>> ToBool(const Value& v, BinaryOp op) {
  if (std::holds_alternative<std::monostate>(v)) {
    return std::nullopt;
  }
  if (const auto* b = std::get_if<bool>(&v)) {
    return *b;
  }
  return Error::TypeError("'" + std::string(plan::ToString(op)) + "' needs bool operands");
}

/// @brief Three-valued AND/OR: the dominant value (false for AND, true for OR) wins over
/// null; otherwise any null makes the result null.
[[nodiscard]] Result<std::optional<bool>> AndOr(BinaryOp op, const Value& a, const Value& b) {
  auto lhs = ToBool(a, op);
  if (!lhs) {
    return lhs;
  }
  auto rhs = ToBool(b, op);
  if (!rhs) {
    return rhs;
  }
  const bool dominant = op == BinaryOp::kOr;
  if (*lhs == dominant || *rhs == dominant) {
    return dominant;
  }
  if (!lhs->has_value() || !rhs->has_value()) {
    return std::nullopt;
  }
  return !dominant;
}

template <typename T>
[[nodiscard]] T NumericAs(const Value& v) {
  return std::visit(
      [](const auto& x) -> T {
        using V = std::decay_t<decltype(x)>;
        if constexpr (std::is_arithmetic_v<V> && !std::is_same_v<V, bool>) {
          return static_cast<T>(x);
        } else {
          // BinaryResultType admitted only numeric column types.
          std::unreachable();
        }
      },
      v);
}

/// @brief One row of `a op b` computed in the result type @p T.
template <typename T>
[[nodiscard]] Result<std::optional<T>> Arithmetic(BinaryOp op, const Value& a, const Value& b) {
  if (std::holds_alternative<std::monostate>(a) || std::holds_alternative<std::monostate>(b)) {
    return std::nullopt;
  }
  const T x = NumericAs<T>(a);
  const T y = NumericAs<T>(b);
  if constexpr (std::is_integral_v<T>) {
    T out{};
    bool overflow = false;
    switch (op) {
      case BinaryOp::kAdd:
        overflow = __builtin_add_overflow(x, y, &out);
        break;
      case BinaryOp::kSubtract:
        overflow = __builtin_sub_overflow(x, y, &out);
        break;
      case BinaryOp::kMultiply:
        overflow = __builtin_mul_overflow(x, y, &out);
        break;
      case BinaryOp::kModulo:
        if (y == 0) {
          return std::nullopt;
        }
        // min % -1 overflows in C++; the mathematical result is 0.
        out = y == -1 ? T{0} : static_cast<T>(x % y);
        break;
      default:
        std::unreachable();  // integer `/` yields double
    }
    if (overflow) {
      return Error::OutOfRange("Integer overflow in '" + std::string(plan::ToString(op)) + "'");
    }
    return out;
  } else {
    switch (op) {
      case BinaryOp::kAdd:
        return x + y;
      case BinaryOp::kSubtract:
        return x - y;
      case BinaryOp::kMultiply:
        return x * y;
      case BinaryOp::kDivide:
        return y == 0 ? std::optional<T>{} : x / y;
      case BinaryOp::kModulo:
        return y == 0 ? std::optional<T>{} : std::fmod(x, y);
      default:
        std::unreachable();
    }
  }
}

/// @brief True for a NULL literal: only literals are constant columns.
[[nodiscard]] bool IsNullLiteral(const ColumnVector& col) {
  return col.is_constant() && col.IsNull(0);
}

[[nodiscard]] Result<DataType> OperandResultType(BinaryOp op, const ColumnVector& l,
                                                 const ColumnVector& r) {
  return BinaryResultType(op, IsNullLiteral(l) ? r.GetType() : l.GetType(),
                          IsNullLiteral(r) ? l.GetType() : r.GetType());
}

[[nodiscard]] Result<ColumnVectorPtr> EvaluateArithmetic(BinaryOp op,
                                                                       const ColumnVector& l,
                                                                       const ColumnVector& r,
                                                                       size_t n) {
  auto type = OperandResultType(op, l, r);
  if (!type) {
    return std::unexpected(type.error());
  }
  return columnar::VisitType(*type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = typename Tag::CType;
    if constexpr (std::is_arithmetic_v<T> && !std::same_as<T, bool>) {
      return MapRows<T>(l, r, n, *type,
                        [op](const Value& a, const Value& b) { return Arithmetic<T>(op, a, b); });
    } else {
      std::unreachable();  // BinaryResultType yields only numeric types for arithmetic
    }
  });
}

/// @brief @p x as @p T; the caller has checked that the types are equal or both numeric.
template <typename T, typename S>
[[nodiscard]] T ConvertTo(const S& x) {
  if constexpr (std::is_same_v<S, T>) {
    return x;
  } else if constexpr (std::is_arithmetic_v<S> && !std::is_same_v<S, bool> &&
                       std::is_arithmetic_v<T> && !std::is_same_v<T, bool>) {
    return static_cast<T>(x);
  } else {
    std::unreachable();
  }
}

/// @brief A CASE operand converted to type @p T in plain vectors, so rows are picked without
/// virtual calls. A broadcast lane holds a single row that stands for every row.
template <typename T>
struct Lane {
  std::vector<T> values;
  std::vector<bool> validity;  // empty: no row is null
  bool broadcast = false;

  [[nodiscard]] size_t Row(size_t i) const { return broadcast ? 0 : i; }
  [[nodiscard]] bool Valid(size_t i) const { return validity.empty() || validity[Row(i)]; }
};

/// @brief Converts @p col to a `Lane<T>`; a literal becomes a one-row broadcast lane.
template <typename T>
[[nodiscard]] Lane<T> ToLane(const ColumnVector& col, size_t num_rows) {
  Lane<T> lane;
  lane.broadcast = col.is_constant();
  const size_t rows = lane.broadcast ? 1 : num_rows;
  lane.values.resize(rows);
  lane.validity.assign(rows, true);
  bool has_null = false;
  for (size_t i = 0; i < rows; ++i) {
    const Value v = col.GetValue(i);
    if (std::holds_alternative<std::monostate>(v)) {
      lane.validity[i] = false;
      has_null = true;
    } else {
      lane.values[i] = std::visit([](const auto& x) { return ConvertTo<T>(x); }, v);
    }
  }
  if (!has_null) {
    lane.validity.clear();
  }
  return lane;
}

/// @brief Per row, copies `results[b]` for the first `whens[b]` that is true, else
/// `results.back()` (the ELSE).
/// @tparam T The result's CType.
template <typename T>
[[nodiscard]] ColumnVectorPtr SelectRows(const std::vector<ColumnVectorPtr>& whens,
                                         const std::vector<ColumnVectorPtr>& results,
                                         size_t num_rows, DataType type) {
  std::vector<Lane<bool>> conditions;
  conditions.reserve(whens.size());
  for (const auto& when : whens) {
    conditions.push_back(ToLane<bool>(*when, num_rows));
  }
  std::vector<Lane<columnar::ValueOf<T>>> lanes;
  lanes.reserve(results.size());
  for (const auto& result : results) {
    lanes.push_back(ToLane<columnar::ValueOf<T>>(*result, num_rows));
  }

  columnar::ColumnBuilder<T> builder(type, num_rows);
  for (size_t i = 0; i < num_rows; ++i) {
    size_t pick = conditions.size();
    for (size_t b = 0; b < conditions.size(); ++b) {
      const Lane<bool>& c = conditions[b];
      if (c.Valid(i) && c.values[c.Row(i)]) {
        pick = b;
        break;
      }
    }
    const auto& lane = lanes[pick];
    if (lane.Valid(i)) {
      builder.Append(lane.values[lane.Row(i)]);
    } else {
      builder.AppendNull();
    }
  }
  return std::move(builder).Finish();
}

}  // namespace

// =============================================================================
// Literal Expression
// =============================================================================

Result<ColumnVectorPtr> LiteralExpression::Evaluate(
    const RecordBatch& input) const {
  DataType dtype = std::visit(
      [](const auto& val) -> DataType {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, int8_t>) {
          return columnar::DataType::kInt8;
        } else if constexpr (std::is_same_v<T, int16_t>) {
          return columnar::DataType::kInt16;
        } else if constexpr (std::is_same_v<T, int32_t>) {
          return columnar::DataType::kInt32;
        } else if constexpr (std::is_same_v<T, int64_t>) {
          return columnar::DataType::kInt64;
        } else if constexpr (std::is_same_v<T, float>) {
          return columnar::DataType::kFloat;
        } else if constexpr (std::is_same_v<T, double>) {
          return columnar::DataType::kDouble;
        } else if constexpr (std::is_same_v<T, bool>) {
          return columnar::DataType::kBool;
        } else if constexpr (std::is_same_v<T, columnar::Date32>) {
          return columnar::DataType::kDate32;
        } else if constexpr (std::is_same_v<T, columnar::Timestamp>) {
          return columnar::DataType::kTimestamp;
        } else {
          return columnar::DataType::kString;
        }
      },
      value_);
  auto one_row = columnar::ColumnFromValues(dtype, std::span<const Value>(&value_, 1));
  if (!one_row) {
    return std::unexpected(one_row.error());
  }
  return ColumnVector::Constant(**one_row, input.num_rows());
}

std::string LiteralExpression::ToString() const {
  return std::visit(
      [](const auto& val) -> std::string {
        using T = std::decay_t<decltype(val)>;
        if constexpr (std::is_same_v<T, std::string>) {
          return "\"" + val + "\"";
        } else if constexpr (std::is_same_v<T, bool>) {
          return val ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          return "<binary>";
        } else if constexpr (std::is_same_v<T, columnar::Date32>) {
          return "DATE '" + columnar::ValueToString(val) + "'";
        } else if constexpr (std::is_same_v<T, columnar::Timestamp>) {
          return "TIMESTAMP '" + columnar::ValueToString(val) + "'";
        } else {
          return std::to_string(val);
        }
      },
      value_);
}

// =============================================================================
// Column Expression
// =============================================================================

Result<ColumnVectorPtr> ColumnExpression::Evaluate(
    const RecordBatch& input) const {
  if (index_ >= input.num_columns()) {
    return std::unexpected(
        Error::OutOfRange("Column index out of range: " +
                           std::to_string(index_)));
  }
  return input.column_ptr(index_);
}

// =============================================================================
// Binary Expression
// =============================================================================

Result<ColumnVectorPtr> BinaryExpression::Evaluate(
    const RecordBatch& input) const {
  auto left_result = left_->Evaluate(input);
  if (!left_result) {
    return std::unexpected(left_result.error());
  }
  auto right_result = right_->Evaluate(input);
  if (!right_result) {
    return std::unexpected(right_result.error());
  }

  const auto& l = **left_result;
  const auto& r = **right_result;
  const size_t n = input.num_rows();

  auto compare = [&]<typename Op>() -> Result<ColumnVectorPtr> {
    if (auto type = OperandResultType(op_, l, r); !type) {
      return std::unexpected(type.error());
    }
    return MapRows<bool>(l, r, n, columnar::DataType::kBool, CompareValues<Op>);
  };
  switch (op_) {
    case BinaryOp::kEq:
      return compare.template operator()<std::equal_to<>>();
    case BinaryOp::kNeq:
      return compare.template operator()<std::not_equal_to<>>();
    case BinaryOp::kLt:
      return compare.template operator()<std::less<>>();
    case BinaryOp::kLtEq:
      return compare.template operator()<std::less_equal<>>();
    case BinaryOp::kGt:
      return compare.template operator()<std::greater<>>();
    case BinaryOp::kGtEq:
      return compare.template operator()<std::greater_equal<>>();
    case BinaryOp::kAnd:
    case BinaryOp::kOr:
      return MapRows<bool>(l, r, n, columnar::DataType::kBool,
                           [op = op_](const Value& a, const Value& b) { return AndOr(op, a, b); });
    case BinaryOp::kAdd:
    case BinaryOp::kSubtract:
    case BinaryOp::kMultiply:
    case BinaryOp::kDivide:
    case BinaryOp::kModulo:
      return EvaluateArithmetic(op_, l, r, n);
  }
  std::unreachable();
}

std::string BinaryExpression::ToString() const {
  std::string s =
      left_->ToString() + " " + std::string(plan::ToString(op_)) + " " + right_->ToString();
  return IsArithmetic(op_) ? "(" + s + ")" : s;
}

// =============================================================================
// CASE Expression
// =============================================================================

Result<ColumnVectorPtr> CaseExpression::Evaluate(const RecordBatch& input) const {
  std::vector<ColumnVectorPtr> whens;
  std::vector<ColumnVectorPtr> results;
  whens.reserve(branches_.size());
  results.reserve(branches_.size() + 1);
  for (const auto& [when, then] : branches_) {
    auto condition = when->Evaluate(input);
    if (!condition) {
      return std::unexpected(condition.error());
    }
    const DataType condition_type = (*condition)->GetType();
    if (!IsNullLiteral(**condition) &&
        !(condition_type == columnar::DataType::kBool)) {
      return Error::TypeError("CASE WHEN needs a bool condition, got " +
                              columnar::GetTypeName(condition_type));
    }
    whens.push_back(std::move(*condition));
    auto result = then->Evaluate(input);
    if (!result) {
      return std::unexpected(result.error());
    }
    results.push_back(std::move(*result));
  }
  auto otherwise = otherwise_->Evaluate(input);
  if (!otherwise) {
    return std::unexpected(otherwise.error());
  }
  results.push_back(std::move(*otherwise));

  std::optional<DataType> type;
  for (const auto& result : results) {
    if (IsNullLiteral(*result)) {
      continue;
    }
    if (!type) {
      type = result->GetType();
      continue;
    }
    auto common = CommonType(*type, result->GetType());
    if (!common) {
      return std::unexpected(common.error());
    }
    type = std::move(*common);
  }
  if (!type) {
    type = results.back()->GetType();  // every result is a NULL literal
  }

  const size_t n = input.num_rows();
  return columnar::VisitType(*type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    return SelectRows<typename Tag::CType>(whens, results, n, *type);
  });
}

std::string CaseExpression::ToString() const {
  std::string s = "CASE";
  for (const auto& [when, then] : branches_) {
    s += " WHEN " + when->ToString() + " THEN " + then->ToString();
  }
  return s + " ELSE " + otherwise_->ToString() + " END";
}

// =============================================================================
// Aggregate Expressions
// =============================================================================

double AggregateExpression::ToDouble(const Value& val) {
  return std::visit(
      [](const auto& v) -> double {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_arithmetic_v<T>) {
          return static_cast<double>(v);
        } else {
          return 0.0;
        }
      },
      val);
}

Result<ColumnVectorPtr> SumExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  double sum = 0.0;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      sum += ToDouble(col.GetValue(i));
    }
  }

  return OneRow<double>(columnar::DataType::kDouble, sum);
}

Result<ColumnVectorPtr> AvgExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  double sum = 0.0;
  size_t count = 0;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      sum += ToDouble(col.GetValue(i));
      ++count;
    }
  }

  double avg = (count > 0) ? (sum / static_cast<double>(count)) : 0.0;

  return OneRow<double>(columnar::DataType::kDouble, avg);
}

Result<ColumnVectorPtr> CountExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  int64_t count = 0;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      ++count;
    }
  }

  return OneRow<int64_t>(columnar::DataType::kInt64, count);
}

Result<ColumnVectorPtr> CountStarExpression::Evaluate(
    const RecordBatch& input) const {
  return OneRow<int64_t>(columnar::DataType::kInt64, static_cast<int64_t>(input.num_rows()));
}

Result<ColumnVectorPtr> MinExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  if (col.size() == 0) {
    return OneRow<double>(columnar::DataType::kDouble, std::numeric_limits<double>::quiet_NaN());
  }

  double min_val = std::numeric_limits<double>::max();
  bool found = false;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      double val = ToDouble(col.GetValue(i));
      if (!found || val < min_val) {
        min_val = val;
        found = true;
      }
    }
  }

  if (!found) {
    min_val = std::numeric_limits<double>::quiet_NaN();
  }

  return OneRow<double>(columnar::DataType::kDouble, min_val);
}

Result<ColumnVectorPtr> MaxExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  if (col.size() == 0) {
    return OneRow<double>(columnar::DataType::kDouble, std::numeric_limits<double>::quiet_NaN());
  }

  double max_val = std::numeric_limits<double>::lowest();
  bool found = false;

  for (size_t i = 0; i < col.size(); ++i) {
    if (!col.IsNull(i)) {
      double val = ToDouble(col.GetValue(i));
      if (!found || val > max_val) {
        max_val = val;
        found = true;
      }
    }
  }

  if (!found) {
    max_val = std::numeric_limits<double>::quiet_NaN();
  }

  return OneRow<double>(columnar::DataType::kDouble, max_val);
}

}  // namespace quest::plan::physical
