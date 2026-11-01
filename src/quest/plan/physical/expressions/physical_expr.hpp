// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file physical_expr.hpp
/// @brief Physical expressions for query execution.

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"
#include "quest/plan/binary_op.hpp"

namespace quest::plan::physical {

using columnar::ColumnVector;
using plan::BinaryOp;
using columnar::DataType;
using columnar::Field;
using columnar::RecordBatch;
using columnar::Schema;
using columnar::Value;

// =============================================================================
// Column Data Type (for expression evaluation)
// =============================================================================

/// @brief Variant type for column data storage during expression evaluation.
using ColumnData = std::variant<std::vector<bool>, std::vector<int8_t>,
                                std::vector<int16_t>, std::vector<int32_t>,
                                std::vector<int64_t>, std::vector<float>,
                                std::vector<double>, std::vector<std::string>>;

// =============================================================================
// Materialized Column Vector
// =============================================================================

/// @brief A column vector that owns its data (for expression results).
class MaterializedColumnVector : public ColumnVector {
 public:
  MaterializedColumnVector(ColumnData data, DataType type)
      : data_(std::move(data)), type_(std::move(type)) {}

  [[nodiscard]] DataType GetType() const override { return type_; }

  [[nodiscard]] size_t size() const override {
    return std::visit([](const auto& vec) { return vec.size(); }, data_);
  }

  [[nodiscard]] bool IsNull(size_t /*index*/) const override {
    return false;  // Materialized columns don't track nulls for now
  }

  [[nodiscard]] Value GetValue(size_t index) const override {
    return std::visit(
        [index](const auto& vec) -> Value {
          if (index >= vec.size()) {
            return std::monostate{};
          }
          return vec[index];
        },
        data_);
  }

  [[nodiscard]] const ColumnData& data() const { return data_; }

 private:
  ColumnData data_;
  DataType type_;
};

/// @brief A column vector that broadcasts a single value.
class LiteralColumnVector : public ColumnVector {
 public:
  LiteralColumnVector(Value value, DataType type, size_t count)
      : value_(std::move(value)), type_(std::move(type)), count_(count) {}

  [[nodiscard]] DataType GetType() const override { return type_; }
  [[nodiscard]] size_t size() const override { return count_; }
  [[nodiscard]] bool IsNull(size_t /*index*/) const override {
    return std::holds_alternative<std::monostate>(value_);
  }
  [[nodiscard]] Value GetValue(size_t /*index*/) const override {
    return value_;
  }

  [[nodiscard]] const Value& value() const { return value_; }

 private:
  Value value_;
  DataType type_;
  size_t count_;
};

// =============================================================================
// Abstract Expression Base Class
// =============================================================================

/// @brief Abstract base class for physical expressions.
class Expression {
 public:
  virtual ~Expression() = default;

  [[nodiscard]] virtual Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const = 0;
  [[nodiscard]] virtual std::string ToString() const = 0;
};

using ExpressionPtr = std::unique_ptr<Expression>;

// =============================================================================
// Literal Expression
// =============================================================================

/// @brief Literal expression - returns a constant value for all rows.
class LiteralExpression : public Expression {
 public:
  explicit LiteralExpression(Value value) : value_(std::move(value)) {}

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  [[nodiscard]] std::string ToString() const override {
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

 private:
  Value value_;
};

// =============================================================================
// Column Expression
// =============================================================================

/// @brief Column expression - returns a column from the input batch.
class ColumnExpression : public Expression {
 public:
  explicit ColumnExpression(size_t index) : index_(index) {}

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
    if (index_ >= input.num_columns()) {
      return std::unexpected(
          Error::OutOfRange("Column index out of range: " +
                             std::to_string(index_)));
    }
    return input.column_ptr(index_);
  }

  [[nodiscard]] std::string ToString() const override {
    return "#" + std::to_string(index_);
  }

 private:
  size_t index_;
};

// =============================================================================
// Binary Expression
// =============================================================================

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

/// @brief Binary expression. Comparisons and AND/OR produce a bool column; arithmetic
/// produces a double column.
///
/// AND/OR are false unless both operands are bool. Arithmetic treats non-numeric values
/// (including null) as 0.0; division or modulo by zero yields NaN.
class BinaryExpression : public Expression {
 public:
  BinaryExpression(BinaryOp op, ExpressionPtr left, ExpressionPtr right)
      : op_(op), left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  /// @brief `l op r`; arithmetic is parenthesized, `(l op r)`.
  [[nodiscard]] std::string ToString() const override {
    std::string s =
        left_->ToString() + " " + std::string(plan::ToString(op_)) + " " + right_->ToString();
    return IsArithmetic(op_) ? "(" + s + ")" : s;
  }

  [[nodiscard]] BinaryOp op() const noexcept { return op_; }

 private:
  template <typename T, typename Fn>
  [[nodiscard]] static std::shared_ptr<ColumnVector> MapRows(const ColumnVector& left,
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

  [[nodiscard]] static double ToDouble(const Value& val) {
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

  BinaryOp op_;
  ExpressionPtr left_;
  ExpressionPtr right_;
};

// =============================================================================
// Aggregate Expressions
// =============================================================================

class AggregateExpression : public Expression {
 public:
  explicit AggregateExpression(ExpressionPtr input)
      : input_(std::move(input)) {}

 protected:
  /// @brief Helper to convert Value to double.
  [[nodiscard]] static double ToDouble(const Value& val) {
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

  ExpressionPtr input_;
};

class SumExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  [[nodiscard]] std::string ToString() const override {
    return "SUM(" + input_->ToString() + ")";
  }
};

class AvgExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  [[nodiscard]] std::string ToString() const override {
    return "AVG(" + input_->ToString() + ")";
  }
};

/// @brief COUNT aggregate - counts non-null values.
class CountExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(" + input_->ToString() + ")";
  }
};

/// @brief COUNT(*) aggregate - counts all rows including nulls.
class CountStarExpression : public Expression {
 public:
  CountStarExpression() = default;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
    std::vector<int64_t> result{static_cast<int64_t>(input.num_rows())};
    return std::make_shared<MaterializedColumnVector>(
        ColumnData{std::move(result)}, columnar::Int64Type{});
  }

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(*)";
  }
};

class MinExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  [[nodiscard]] std::string ToString() const override {
    return "MIN(" + input_->ToString() + ")";
  }
};

class MaxExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
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

  [[nodiscard]] std::string ToString() const override {
    return "MAX(" + input_->ToString() + ")";
  }
};

}  // namespace quest::plan::physical
