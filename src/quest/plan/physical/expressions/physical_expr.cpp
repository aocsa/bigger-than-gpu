// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/physical/expressions/physical_expr.hpp"

#include <cmath>
#include <functional>
#include <limits>

namespace quest::plan::physical {

namespace {

/// @brief Compares two values; null or non-numeric mismatched operands compare false, mixed
/// numeric types compare as double.
template <typename Op>
bool CompareValues(const Value& left, const Value& right) {
  return std::visit(
      [](const auto& lhs, const auto& rhs) -> bool {
        using LT = std::decay_t<decltype(lhs)>;
        using RT = std::decay_t<decltype(rhs)>;

        if constexpr (std::is_same_v<LT, std::monostate> ||
                      std::is_same_v<RT, std::monostate>) {
          return false;  // null comparisons are false
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

template <typename T, typename Fn>
[[nodiscard]] std::shared_ptr<ColumnVector> MapRows(const ColumnVector& left,
                                                    const ColumnVector& right,
                                                    size_t num_rows, Fn fn) {
  std::vector<T> result;
  result.reserve(num_rows);
  for (size_t i = 0; i < num_rows; ++i) {
    result.push_back(fn(left.GetValue(i), right.GetValue(i)));
  }
  DataType type = columnar::DoubleType{};
  if constexpr (std::is_same_v<T, bool>) {
    type = columnar::BoolType{};
  }
  return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(result)},
                                                    std::move(type));
}

[[nodiscard]] double ToDouble(const Value& val) {
  return std::visit(
      [](const auto& v) -> double {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_arithmetic_v<T>) {
          return static_cast<double>(v);
        } else {
          return 0.0;  // Non-numeric types return 0
        }
      },
      val);
}

}  // namespace

// =============================================================================
// Materialized Column Vector
// =============================================================================

Value MaterializedColumnVector::GetValue(size_t index) const {
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
        if constexpr (std::is_same_v<T, int32_t>) {
          return columnar::Int32Type{};
        } else if constexpr (std::is_same_v<T, int64_t>) {
          return columnar::Int64Type{};
        } else if constexpr (std::is_same_v<T, float>) {
          return columnar::FloatType{};
        } else if constexpr (std::is_same_v<T, double>) {
          return columnar::DoubleType{};
        } else if constexpr (std::is_same_v<T, bool>) {
          return columnar::BoolType{};
        } else {
          return columnar::StringType{};
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

  switch (op_) {
    case BinaryOp::kEq:
      return MapRows<bool>(l, r, n, CompareValues<std::equal_to<>>);
    case BinaryOp::kNeq:
      return MapRows<bool>(l, r, n, CompareValues<std::not_equal_to<>>);
    case BinaryOp::kLt:
      return MapRows<bool>(l, r, n, CompareValues<std::less<>>);
    case BinaryOp::kLtEq:
      return MapRows<bool>(l, r, n, CompareValues<std::less_equal<>>);
    case BinaryOp::kGt:
      return MapRows<bool>(l, r, n, CompareValues<std::greater<>>);
    case BinaryOp::kGtEq:
      return MapRows<bool>(l, r, n, CompareValues<std::greater_equal<>>);
    case BinaryOp::kAnd:
      return MapRows<bool>(l, r, n, [](const Value& a, const Value& b) {
        const auto* lhs = std::get_if<bool>(&a);
        const auto* rhs = std::get_if<bool>(&b);
        return lhs && rhs && *lhs && *rhs;
      });
    case BinaryOp::kOr:
      return MapRows<bool>(l, r, n, [](const Value& a, const Value& b) {
        const auto* lhs = std::get_if<bool>(&a);
        const auto* rhs = std::get_if<bool>(&b);
        return lhs && rhs && (*lhs || *rhs);
      });
    case BinaryOp::kAdd:
      return MapRows<double>(
          l, r, n, [](const Value& a, const Value& b) { return ToDouble(a) + ToDouble(b); });
    case BinaryOp::kSubtract:
      return MapRows<double>(
          l, r, n, [](const Value& a, const Value& b) { return ToDouble(a) - ToDouble(b); });
    case BinaryOp::kMultiply:
      return MapRows<double>(
          l, r, n, [](const Value& a, const Value& b) { return ToDouble(a) * ToDouble(b); });
    case BinaryOp::kDivide:
      return MapRows<double>(l, r, n, [](const Value& a, const Value& b) {
        double d = ToDouble(b);
        return d == 0.0 ? std::numeric_limits<double>::quiet_NaN() : ToDouble(a) / d;
      });
    case BinaryOp::kModulo:
      return MapRows<double>(l, r, n, [](const Value& a, const Value& b) {
        double d = ToDouble(b);
        return d == 0.0 ? std::numeric_limits<double>::quiet_NaN() : std::fmod(ToDouble(a), d);
      });
  }
  std::unreachable();
}

std::string BinaryExpression::ToString() const {
  std::string s =
      left_->ToString() + " " + std::string(plan::ToString(op_)) + " " + right_->ToString();
  return IsArithmetic(op_) ? "(" + s + ")" : s;
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
      ColumnData{std::move(result)}, columnar::DoubleType{});
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
      ColumnData{std::move(result)}, columnar::DoubleType{});
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
      ColumnData{std::move(result)}, columnar::Int64Type{});
}

Result<std::shared_ptr<ColumnVector>> CountStarExpression::Evaluate(
    const RecordBatch& input) const {
  std::vector<int64_t> result{static_cast<int64_t>(input.num_rows())};
  return std::make_shared<MaterializedColumnVector>(
      ColumnData{std::move(result)}, columnar::Int64Type{});
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
        ColumnData{std::move(result)}, columnar::DoubleType{});
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
      ColumnData{std::move(result)}, columnar::DoubleType{});
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
        ColumnData{std::move(result)}, columnar::DoubleType{});
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
      ColumnData{std::move(result)}, columnar::DoubleType{});
}

}  // namespace quest::plan::physical
