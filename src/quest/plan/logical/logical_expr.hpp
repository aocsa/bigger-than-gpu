// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file logical_expr.hpp
/// @brief Logical expressions for query plans.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"
#include "quest/plan/aggregate.hpp"
#include "quest/plan/binary_op.hpp"

namespace quest::plan::logical {

class LogicalPlan;

using columnar::DataType;
using plan::BinaryOp;
using columnar::Field;
using columnar::Scalar;

// =============================================================================
// Abstract Base Class
// =============================================================================

/// @brief Abstract base class for logical expressions.
class LogicalExpr {
 public:
  virtual ~LogicalExpr() = default;

  [[nodiscard]] virtual Result<Field> ToField(const LogicalPlan& input) const = 0;
  [[nodiscard]] virtual std::string ToString() const = 0;
  [[nodiscard]] virtual std::unique_ptr<LogicalExpr> Clone() const = 0;
};

using LogicalExprPtr = std::unique_ptr<LogicalExpr>;

// =============================================================================
// Column References
// =============================================================================

/// @brief Column reference by name.
class Column : public LogicalExpr {
 public:
  explicit Column(std::string name) : name_(std::move(name)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override;

  [[nodiscard]] std::string ToString() const override { return "#" + name_; }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Column>(name_);
  }

  [[nodiscard]] const std::string& name() const { return name_; }

 private:
  std::string name_;
};

/// @brief Column reference by index.
class ColumnIndex : public LogicalExpr {
 public:
  explicit ColumnIndex(size_t index) : index_(index) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override;

  [[nodiscard]] std::string ToString() const override {
    return "#" + std::to_string(index_);
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<ColumnIndex>(index_);
  }

  [[nodiscard]] size_t index() const { return index_; }

 private:
  size_t index_;
};

// =============================================================================
// Literal Expression
// =============================================================================

class Literal : public LogicalExpr {
 public:
  explicit Literal(Scalar value) : value_(std::move(value)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override;

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Literal>(value_);
  }

  [[nodiscard]] const Scalar& value() const { return value_; }

 private:
  Scalar value_;
};

// =============================================================================
// Binary Expressions
// =============================================================================

/// @brief Binary expression; comparisons and AND/OR yield bool, arithmetic yields
/// `BinaryResultType`.
class BinaryExpr : public LogicalExpr {
 public:
  BinaryExpr(BinaryOp op, LogicalExprPtr left, LogicalExprPtr right)
      : op_(op), left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override;

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<BinaryExpr>(op_, left_->Clone(), right_->Clone());
  }

  [[nodiscard]] BinaryOp op() const noexcept { return op_; }
  [[nodiscard]] const LogicalExpr& left() const { return *left_; }
  [[nodiscard]] const LogicalExpr& right() const { return *right_; }

 private:
  BinaryOp op_;
  LogicalExprPtr left_;
  LogicalExprPtr right_;
};

// =============================================================================
// CASE Expression
// =============================================================================

/// @brief Searched CASE: the THEN of the first WHEN that is true, else `otherwise`.
///
/// Every WHEN must be bool (a null WHEN counts as not true). The result type is the
/// `CommonType` of the THENs and `otherwise`, ignoring NULL literals.
class CaseExpr : public LogicalExpr {
 public:
  /// @brief (WHEN, THEN) pairs in evaluation order.
  using Branches = std::vector<std::pair<LogicalExprPtr, LogicalExprPtr>>;

  /// @param branches (WHEN, THEN) pairs; at least one.
  /// @param otherwise The ELSE expression.
  CaseExpr(Branches branches, LogicalExprPtr otherwise)
      : branches_(std::move(branches)), otherwise_(std::move(otherwise)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override;

  /// @brief `CASE WHEN <c> THEN <a> ... ELSE <b> END`.
  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override;

  [[nodiscard]] const Branches& branches() const noexcept { return branches_; }
  [[nodiscard]] const LogicalExpr& otherwise() const { return *otherwise_; }

 private:
  Branches branches_;
  LogicalExprPtr otherwise_;
};

// =============================================================================
// Aggregate Expression
// =============================================================================

/// @brief `SUM(x)`, `COUNT(x)`, `COUNT(*)`, `MIN(x)`, `MAX(x)` or `AVG(x)`.
class AggregateExpr final : public LogicalExpr {
 public:
  /// @param kind The aggregate function.
  /// @param input The argument; nullptr for `COUNT(*)`.
  AggregateExpr(AggregateKind kind, LogicalExprPtr input)
      : kind_(kind), input_(std::move(input)) {}

  /// @return Int64 for COUNT, Double for AVG, `SumType` for SUM (`TypeError` for a
  /// non-numeric input), the input type for MIN and MAX.
  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override;

  [[nodiscard]] std::string ToString() const override;

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<AggregateExpr>(kind_, input_ ? input_->Clone() : nullptr);
  }

  [[nodiscard]] AggregateKind kind() const noexcept { return kind_; }
  /// @return The argument; nullptr for `COUNT(*)`.
  [[nodiscard]] const LogicalExpr* input() const noexcept { return input_.get(); }

 private:
  AggregateKind kind_;
  LogicalExprPtr input_;
};

// =============================================================================
// Factory Functions
// =============================================================================

inline LogicalExprPtr col(const std::string& name) {
  return std::make_unique<Column>(name);
}

inline LogicalExprPtr col_idx(size_t index) {
  return std::make_unique<ColumnIndex>(index);
}

template <typename T>
LogicalExprPtr lit(T value) {
  return std::make_unique<Literal>(Scalar(value));
}

inline LogicalExprPtr eq(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kEq, std::move(left), std::move(right));
}

inline LogicalExprPtr neq(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kNeq, std::move(left), std::move(right));
}

inline LogicalExprPtr gt(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kGt, std::move(left), std::move(right));
}

inline LogicalExprPtr gte(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kGtEq, std::move(left), std::move(right));
}

inline LogicalExprPtr lt(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kLt, std::move(left), std::move(right));
}

inline LogicalExprPtr lte(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kLtEq, std::move(left), std::move(right));
}

inline LogicalExprPtr and_expr(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kAnd, std::move(left), std::move(right));
}

inline LogicalExprPtr or_expr(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kOr, std::move(left), std::move(right));
}

inline LogicalExprPtr add(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kAdd, std::move(left), std::move(right));
}

inline LogicalExprPtr subtract(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kSubtract, std::move(left), std::move(right));
}

inline LogicalExprPtr multiply(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kMultiply, std::move(left), std::move(right));
}

inline LogicalExprPtr divide(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kDivide, std::move(left), std::move(right));
}

inline LogicalExprPtr modulo(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<BinaryExpr>(BinaryOp::kModulo, std::move(left), std::move(right));
}

/// @brief `CASE WHEN when THEN then ELSE otherwise END`.
inline LogicalExprPtr case_when(LogicalExprPtr when, LogicalExprPtr then,
                                LogicalExprPtr otherwise) {
  CaseExpr::Branches branches;
  branches.emplace_back(std::move(when), std::move(then));
  return std::make_unique<CaseExpr>(std::move(branches), std::move(otherwise));
}

// =============================================================================
// Aggregate Factory Functions
// =============================================================================

inline LogicalExprPtr sum(LogicalExprPtr input) {
  return std::make_unique<AggregateExpr>(AggregateKind::kSum, std::move(input));
}

inline LogicalExprPtr avg(LogicalExprPtr input) {
  return std::make_unique<AggregateExpr>(AggregateKind::kAvg, std::move(input));
}

inline LogicalExprPtr count(LogicalExprPtr input) {
  return std::make_unique<AggregateExpr>(AggregateKind::kCount, std::move(input));
}

inline LogicalExprPtr count_star() {
  return std::make_unique<AggregateExpr>(AggregateKind::kCountStar, nullptr);
}

inline LogicalExprPtr min(LogicalExprPtr input) {
  return std::make_unique<AggregateExpr>(AggregateKind::kMin, std::move(input));
}

inline LogicalExprPtr max(LogicalExprPtr input) {
  return std::make_unique<AggregateExpr>(AggregateKind::kMax, std::move(input));
}

}  // namespace quest::plan::logical
