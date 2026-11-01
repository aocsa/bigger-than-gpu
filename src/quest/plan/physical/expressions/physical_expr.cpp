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

/// @brief Applies @p fn to each row pair; a `std::nullopt` row result is stored as null.
template <typename T, typename Fn>
[[nodiscard]] Result<std::shared_ptr<ColumnVector>> MapRows(const ColumnVector& left,
                                                            const ColumnVector& right,
                                                            size_t num_rows, DataType type,
                                                            Fn fn) {
  std::vector<T> values(num_rows);
  std::vector<bool> validity(num_rows, true);
  bool has_null = false;
  for (size_t i = 0; i < num_rows; ++i) {
    Result<std::optional<T>> row = fn(left.GetValue(i), right.GetValue(i));
    if (!row) {
      return std::unexpected(row.error());
    }
    if (*row) {
      values[i] = **row;
    } else {
      validity[i] = false;
      has_null = true;
    }
  }
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(values)}, std::move(type),
      has_null ? std::move(validity) : std::vector<bool>{});
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

[[nodiscard]] bool IsNullLiteral(const ColumnVector& col) {
  const auto* lit = dynamic_cast<const LiteralColumnVector*>(&col);
  return lit != nullptr && std::holds_alternative<std::monostate>(lit->value());
}

[[nodiscard]] Result<DataType> OperandResultType(BinaryOp op, const ColumnVector& l,
                                                 const ColumnVector& r) {
  return BinaryResultType(op, IsNullLiteral(l) ? r.GetType() : l.GetType(),
                          IsNullLiteral(r) ? l.GetType() : r.GetType());
}

[[nodiscard]] Result<std::shared_ptr<ColumnVector>> EvaluateArithmetic(BinaryOp op,
                                                                       const ColumnVector& l,
                                                                       const ColumnVector& r,
                                                                       size_t n) {
  auto type = OperandResultType(op, l, r);
  if (!type) {
    return std::unexpected(type.error());
  }
  return columnar::VisitType(*type, [&]<typename Tag>(Tag) -> Result<std::shared_ptr<ColumnVector>> {
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

/// @brief Converts @p col to a `Lane<T>`. Literals and expression results are converted in
/// bulk; other columns (e.g. scanned ones) go through `GetValue`.
template <typename T>
[[nodiscard]] Lane<T> ToLane(const ColumnVector& col, size_t num_rows) {
  Lane<T> lane;
  if (const auto* lit = dynamic_cast<const LiteralColumnVector*>(&col)) {
    lane.broadcast = true;
    if (std::holds_alternative<std::monostate>(lit->value())) {
      lane.values.emplace_back();
      lane.validity.push_back(false);
    } else {
      lane.values.push_back(
          std::visit([](const auto& x) { return ConvertTo<T>(x); }, lit->value()));
    }
    return lane;
  }
  if (const auto* mat = dynamic_cast<const MaterializedColumnVector*>(&col)) {
    std::visit(
        [&lane](const auto& src) {
          // Explicit source type: vector<bool> iterates proxies, not bools.
          using S = typename std::decay_t<decltype(src)>::value_type;
          lane.values.reserve(src.size());
          for (const auto& x : src) {
            lane.values.push_back(ConvertTo<T, S>(x));
          }
        },
        mat->data());
    lane.validity = mat->validity();
    return lane;
  }
  lane.values.resize(num_rows);
  lane.validity.assign(num_rows, true);
  bool has_null = false;
  for (size_t i = 0; i < num_rows; ++i) {
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
template <typename T>
[[nodiscard]] std::shared_ptr<ColumnVector> SelectRows(
    const std::vector<std::shared_ptr<ColumnVector>>& whens,
    const std::vector<std::shared_ptr<ColumnVector>>& results, size_t num_rows, DataType type) {
  std::vector<Lane<bool>> conditions;
  conditions.reserve(whens.size());
  for (const auto& when : whens) {
    conditions.push_back(ToLane<bool>(*when, num_rows));
  }
  std::vector<Lane<T>> lanes;
  lanes.reserve(results.size());
  for (const auto& result : results) {
    lanes.push_back(ToLane<T>(*result, num_rows));
  }

  std::vector<T> values(num_rows);
  std::vector<bool> validity(num_rows, true);
  bool has_null = false;
  for (size_t i = 0; i < num_rows; ++i) {
    size_t pick = conditions.size();
    for (size_t b = 0; b < conditions.size(); ++b) {
      const Lane<bool>& c = conditions[b];
      if (c.Valid(i) && c.values[c.Row(i)]) {
        pick = b;
        break;
      }
    }
    const Lane<T>& lane = lanes[pick];
    if (lane.Valid(i)) {
      values[i] = lane.values[lane.Row(i)];
    } else {
      validity[i] = false;
      has_null = true;
    }
  }
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(values)}, std::move(type),
      has_null ? std::move(validity) : std::vector<bool>{});
}

}  // namespace

// =============================================================================
// Materialized Column Vector
// =============================================================================

Value MaterializedColumnVector::GetValue(size_t index) const {
  if (IsNull(index)) {
    return std::monostate{};
  }
  return std::visit(
      [index](const auto& vec) -> Value {
        if (index >= vec.size()) {
          return std::monostate{};
        }
        return vec[index];
      },
      data_);
}

// =============================================================================
// Literal Expression
// =============================================================================

Result<std::shared_ptr<ColumnVector>> LiteralExpression::Evaluate(
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
  return std::make_shared<LiteralColumnVector>(value_, dtype, input.num_rows());
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

Result<std::shared_ptr<ColumnVector>> ColumnExpression::Evaluate(
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

Result<std::shared_ptr<ColumnVector>> BinaryExpression::Evaluate(
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

  auto compare = [&]<typename Op>() -> Result<std::shared_ptr<ColumnVector>> {
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

Result<std::shared_ptr<ColumnVector>> CaseExpression::Evaluate(const RecordBatch& input) const {
  std::vector<std::shared_ptr<ColumnVector>> whens;
  std::vector<std::shared_ptr<ColumnVector>> results;
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
  return columnar::VisitType(*type, [&]<typename Tag>(Tag) -> Result<std::shared_ptr<ColumnVector>> {
    return SelectRows<columnar::ValueOf<typename Tag::CType>>(whens, results, n, *type);
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

Result<std::shared_ptr<ColumnVector>> SumExpression::Evaluate(
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

  std::vector<double> result{sum};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::DataType::kDouble);
}

Result<std::shared_ptr<ColumnVector>> AvgExpression::Evaluate(
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

  std::vector<double> result{avg};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::DataType::kDouble);
}

Result<std::shared_ptr<ColumnVector>> CountExpression::Evaluate(
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

  std::vector<int64_t> result{count};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::DataType::kInt64);
}

Result<std::shared_ptr<ColumnVector>> CountStarExpression::Evaluate(
    const RecordBatch& input) const {
  std::vector<int64_t> result{static_cast<int64_t>(input.num_rows())};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::DataType::kInt64);
}

Result<std::shared_ptr<ColumnVector>> MinExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  if (col.size() == 0) {
    std::vector<double> result{std::numeric_limits<double>::quiet_NaN()};
    return std::make_shared<MaterializedColumnVector>(
        ColumnData{std::move(result)}, columnar::DataType::kDouble);
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

  std::vector<double> result{min_val};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::DataType::kDouble);
}

Result<std::shared_ptr<ColumnVector>> MaxExpression::Evaluate(
    const RecordBatch& input) const {
  auto input_result = input_->Evaluate(input);
  if (!input_result) {
    return std::unexpected(input_result.error());
  }

  auto& col = **input_result;
  if (col.size() == 0) {
    std::vector<double> result{std::numeric_limits<double>::quiet_NaN()};
    return std::make_shared<MaterializedColumnVector>(
        ColumnData{std::move(result)}, columnar::DataType::kDouble);
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

  std::vector<double> result{max_val};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::DataType::kDouble);
}

}  // namespace quest::plan::physical
