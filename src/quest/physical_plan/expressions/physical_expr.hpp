// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_PHYSICAL_EXPR_HPP
#define QUEST_PHYSICAL_EXPR_HPP

/// @file physical_expr.hpp
/// @brief Physical expressions for query execution.
///
/// This file defines the physical expression types that operate on
/// RecordBatch data during query execution.

#include <algorithm>
#include <cmath>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/result.hpp"

namespace quest {

// Use types from datasource namespace
using datasource::ColumnVector;
using datasource::DataType;
using datasource::Field;
using datasource::RecordBatch;
using datasource::Schema;
using datasource::Value;

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

  /// @brief Evaluates the expression on a record batch.
  /// @param input The input record batch.
  /// @return A new column vector with the result, or error.
  [[nodiscard]] virtual Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const = 0;

  /// @brief Returns a string representation of the expression.
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
            return datasource::Int32Type{};
          } else if constexpr (std::is_same_v<T, int64_t>) {
            return datasource::Int64Type{};
          } else if constexpr (std::is_same_v<T, float>) {
            return datasource::FloatType{};
          } else if constexpr (std::is_same_v<T, double>) {
            return datasource::DoubleType{};
          } else if constexpr (std::is_same_v<T, bool>) {
            return datasource::BoolType{};
          } else {
            return datasource::StringType{};
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
          Status::OutOfRange("Column index out of range: " +
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
// Boolean Expression Base Class
// =============================================================================

/// @brief Base class for binary boolean expressions.
class BooleanExpression : public Expression {
 public:
  BooleanExpression(ExpressionPtr left, ExpressionPtr right)
      : left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override {
    auto left_result = left_->Evaluate(input);
    if (!left_result.has_value()) {
      return std::unexpected(left_result.error());
    }
    auto right_result = right_->Evaluate(input);
    if (!right_result.has_value()) {
      return std::unexpected(right_result.error());
    }

    auto& left_col = *left_result.value();
    auto& right_col = *right_result.value();

    std::vector<bool> result;
    result.reserve(input.num_rows());

    for (size_t i = 0; i < input.num_rows(); ++i) {
      Value left_val = left_col.GetValue(i);
      Value right_val = right_col.GetValue(i);
      result.push_back(EvaluatePair(left_val, right_val));
    }

    return std::make_shared<MaterializedColumnVector>(
        ColumnData{std::move(result)}, datasource::BoolType{});
  }

 protected:
  /// @brief Evaluates the boolean operation on a pair of values.
  [[nodiscard]] virtual bool EvaluatePair(const Value& left,
                                          const Value& right) const = 0;

  ExpressionPtr left_;
  ExpressionPtr right_;
};

// =============================================================================
// Comparison Helper
// =============================================================================

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

// =============================================================================
// Comparison Expressions
// =============================================================================

class EqExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " == " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    return CompareValues<std::equal_to<>>(left, right);
  }
};

class NeqExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " != " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    return CompareValues<std::not_equal_to<>>(left, right);
  }
};

class LtExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " < " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    return CompareValues<std::less<>>(left, right);
  }
};

class LtEqExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " <= " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    return CompareValues<std::less_equal<>>(left, right);
  }
};

class GtExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " > " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    return CompareValues<std::greater<>>(left, right);
  }
};

class GtEqExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " >= " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    return CompareValues<std::greater_equal<>>(left, right);
  }
};

// =============================================================================
// Logical Expressions
// =============================================================================

class AndExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " AND " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    auto lhs = std::get_if<bool>(&left);
    auto rhs = std::get_if<bool>(&right);
    if (lhs && rhs) {
      return *lhs && *rhs;
    }
    return false;
  }
};

class OrExpression : public BooleanExpression {
 public:
  using BooleanExpression::BooleanExpression;

  [[nodiscard]] std::string ToString() const override {
    return left_->ToString() + " OR " + right_->ToString();
  }

 protected:
  [[nodiscard]] bool EvaluatePair(const Value& left,
                                  const Value& right) const override {
    auto lhs = std::get_if<bool>(&left);
    auto rhs = std::get_if<bool>(&right);
    if (lhs && rhs) {
      return *lhs || *rhs;
    }
    return false;
  }
};

}  // namespace quest

#endif  // QUEST_PHYSICAL_EXPR_HPP
