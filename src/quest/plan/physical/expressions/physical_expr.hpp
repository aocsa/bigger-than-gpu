// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file physical_expr.hpp
/// @brief Physical expressions for query execution.

#include <memory>
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
  /// @param validity Per-row valid flag; empty means no row is null.
  MaterializedColumnVector(ColumnData data, DataType type, std::vector<bool> validity = {})
      : data_(std::move(data)), type_(std::move(type)), validity_(std::move(validity)) {}

  [[nodiscard]] DataType GetType() const override { return type_; }

  [[nodiscard]] size_t size() const override {
    return std::visit([](const auto& vec) { return vec.size(); }, data_);
  }

  [[nodiscard]] bool IsNull(size_t index) const override {
    return index < validity_.size() && !validity_[index];
  }

  [[nodiscard]] Value GetValue(size_t index) const override;

  [[nodiscard]] const ColumnData& data() const { return data_; }

 private:
  ColumnData data_;
  DataType type_;
  std::vector<bool> validity_;
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
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override;

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
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "#" + std::to_string(index_);
  }

 private:
  size_t index_;
};

// =============================================================================
// Binary Expression
// =============================================================================

/// @brief Binary expression with SQL null semantics; the result type is `BinaryResultType`.
///
/// A comparison or arithmetic with a null operand is null. AND/OR use three-valued logic:
/// `false AND null` is false, `true OR null` is true, otherwise null wins. Division or
/// modulo by zero is null and integer overflow is an error, as in DuckDB.
class BinaryExpression : public Expression {
 public:
  BinaryExpression(BinaryOp op, ExpressionPtr left, ExpressionPtr right)
      : op_(op), left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  /// @brief `l op r`; arithmetic is parenthesized, `(l op r)`.
  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] BinaryOp op() const noexcept { return op_; }

 private:
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
  [[nodiscard]] static double ToDouble(const Value& val);

  ExpressionPtr input_;
};

class SumExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "SUM(" + input_->ToString() + ")";
  }
};

class AvgExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "AVG(" + input_->ToString() + ")";
  }
};

/// @brief COUNT aggregate - counts non-null values.
class CountExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(" + input_->ToString() + ")";
  }
};

/// @brief COUNT(*) aggregate - counts all rows including nulls.
class CountStarExpression : public Expression {
 public:
  CountStarExpression() = default;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(*)";
  }
};

class MinExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "MIN(" + input_->ToString() + ")";
  }
};

class MaxExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "MAX(" + input_->ToString() + ")";
  }
};

}  // namespace quest::plan::physical
