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
using columnar::ColumnVectorPtr;
using plan::BinaryOp;
using columnar::DataType;
using columnar::Field;
using columnar::RecordBatch;
using columnar::Schema;
using columnar::Value;

// =============================================================================
// Abstract Expression Base Class
// =============================================================================

/// @brief Abstract base class for physical expressions.
class Expression {
 public:
  virtual ~Expression() = default;

  [[nodiscard]] virtual Result<ColumnVectorPtr> Evaluate(
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

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
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

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
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

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
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
// CASE Expression
// =============================================================================

/// @brief Searched CASE: per row, the THEN of the first WHEN that is true, else `otherwise`.
///
/// A null WHEN counts as not true, as in SQL. Every operand is evaluated once per batch and
/// rows are then picked from the results; the result type is the `CommonType` of the THENs
/// and `otherwise` (NULL literals ignored) and a null in the picked operand stays null.
class CaseExpression : public Expression {
 public:
  /// @brief (WHEN, THEN) pairs in evaluation order.
  using Branches = std::vector<std::pair<ExpressionPtr, ExpressionPtr>>;

  /// @param branches (WHEN, THEN) pairs; at least one.
  /// @param otherwise The ELSE expression.
  CaseExpression(Branches branches, ExpressionPtr otherwise)
      : branches_(std::move(branches)), otherwise_(std::move(otherwise)) {}

  /// @return The selected values, or `TypeError` for a non-bool WHEN or incompatible results.
  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  /// @brief `CASE WHEN <c> THEN <a> ... ELSE <b> END`.
  [[nodiscard]] std::string ToString() const override;

 private:
  Branches branches_;
  ExpressionPtr otherwise_;
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

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "SUM(" + input_->ToString() + ")";
  }
};

class AvgExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "AVG(" + input_->ToString() + ")";
  }
};

/// @brief COUNT aggregate - counts non-null values.
class CountExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(" + input_->ToString() + ")";
  }
};

/// @brief COUNT(*) aggregate - counts all rows including nulls.
class CountStarExpression : public Expression {
 public:
  CountStarExpression() = default;

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(*)";
  }
};

class MinExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "MIN(" + input_->ToString() + ")";
  }
};

class MaxExpression : public AggregateExpression {
 public:
  using AggregateExpression::AggregateExpression;

  [[nodiscard]] Result<ColumnVectorPtr> Evaluate(
      const RecordBatch& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "MAX(" + input_->ToString() + ")";
  }
};

}  // namespace quest::plan::physical
