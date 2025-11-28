// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_LOGICAL_EXPR_HPP
#define QUEST_LOGICAL_EXPR_HPP

/// @file logical_expr.hpp
/// @brief Logical expressions for query plans.
///
/// This file defines the expression types used in logical query plans,
/// including column references, literals, and boolean expressions.

#include <memory>
#include <string>
#include <utility>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/result.hpp"

namespace quest {

// Forward declaration
class LogicalPlan;

// Use types from datasource namespace
using datasource::DataType;
using datasource::Field;
using datasource::Value;

// =============================================================================
// Abstract Base Class
// =============================================================================

/// @brief Abstract base class for logical expressions.
class LogicalExpr {
 public:
  virtual ~LogicalExpr() = default;

  /// @brief Derives the output field for this expression.
  /// @param input The input logical plan providing schema context.
  /// @return The output Field on success, or error Status.
  [[nodiscard]] virtual Result<Field> ToField(const LogicalPlan& input) const = 0;

  /// @brief Returns a string representation of the expression.
  [[nodiscard]] virtual std::string ToString() const = 0;

  /// @brief Creates a deep copy of the expression.
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

/// @brief Literal value expression.
class Literal : public LogicalExpr {
 public:
  explicit Literal(Value value) : value_(std::move(value)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    DataType dtype = std::visit(
        [](const auto& v) -> DataType {
          using T = std::decay_t<decltype(v)>;
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
    return Field{"literal", dtype};
  }

  [[nodiscard]] std::string ToString() const override {
    return std::visit(
        [](const auto& v) -> std::string {
          using T = std::decay_t<decltype(v)>;
          if constexpr (std::is_same_v<T, std::string>) {
            return "\"" + v + "\"";
          } else if constexpr (std::is_same_v<T, bool>) {
            return v ? "true" : "false";
          } else if constexpr (std::is_same_v<T, std::monostate>) {
            return "null";
          } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
            return "<binary>";
          } else {
            return std::to_string(v);
          }
        },
        value_);
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Literal>(value_);
  }

  [[nodiscard]] const Value& value() const { return value_; }

 private:
  Value value_;
};

// =============================================================================
// Binary Expressions
// =============================================================================

/// @brief Base class for binary expressions.
class BinaryExpr : public LogicalExpr {
 public:
  BinaryExpr(LogicalExprPtr left, LogicalExprPtr right, std::string op)
      : left_(std::move(left)), right_(std::move(right)), op_(std::move(op)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    return Field{left_->ToString() + " " + op_ + " " + right_->ToString(),
                 datasource::BoolType{}};
  }

  [[nodiscard]] std::string ToString() const override {
    return "(" + left_->ToString() + " " + op_ + " " + right_->ToString() + ")";
  }

  [[nodiscard]] const LogicalExpr& left() const { return *left_; }
  [[nodiscard]] const LogicalExpr& right() const { return *right_; }
  [[nodiscard]] const std::string& op() const { return op_; }

 protected:
  LogicalExprPtr left_;
  LogicalExprPtr right_;
  std::string op_;
};

/// @brief Base class for boolean binary expressions.
class BooleanBinaryExpr : public BinaryExpr {
 public:
  using BinaryExpr::BinaryExpr;
};

// =============================================================================
// Comparison Operators
// =============================================================================

class Eq : public BooleanBinaryExpr {
 public:
  Eq(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), "==") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Eq>(left_->Clone(), right_->Clone());
  }
};

class Neq : public BooleanBinaryExpr {
 public:
  Neq(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), "!=") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Neq>(left_->Clone(), right_->Clone());
  }
};

class Gt : public BooleanBinaryExpr {
 public:
  Gt(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), ">") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Gt>(left_->Clone(), right_->Clone());
  }
};

class GtEq : public BooleanBinaryExpr {
 public:
  GtEq(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), ">=") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<GtEq>(left_->Clone(), right_->Clone());
  }
};

class Lt : public BooleanBinaryExpr {
 public:
  Lt(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), "<") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Lt>(left_->Clone(), right_->Clone());
  }
};

class LtEq : public BooleanBinaryExpr {
 public:
  LtEq(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), "<=") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<LtEq>(left_->Clone(), right_->Clone());
  }
};

// =============================================================================
// Logical Operators
// =============================================================================

class And : public BooleanBinaryExpr {
 public:
  And(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), "AND") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<And>(left_->Clone(), right_->Clone());
  }
};

class Or : public BooleanBinaryExpr {
 public:
  Or(LogicalExprPtr left, LogicalExprPtr right)
      : BooleanBinaryExpr(std::move(left), std::move(right), "OR") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Or>(left_->Clone(), right_->Clone());
  }
};

// =============================================================================
// Factory Functions
// =============================================================================

/// @brief Create a column reference by name.
inline LogicalExprPtr col(const std::string& name) {
  return std::make_unique<Column>(name);
}

/// @brief Create a column reference by index.
inline LogicalExprPtr col_idx(size_t index) {
  return std::make_unique<ColumnIndex>(index);
}

/// @brief Create a literal value.
template <typename T>
LogicalExprPtr lit(T value) {
  return std::make_unique<Literal>(Value{value});
}

/// @brief Create an equality expression.
inline LogicalExprPtr eq(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Eq>(std::move(left), std::move(right));
}

/// @brief Create an inequality expression.
inline LogicalExprPtr neq(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Neq>(std::move(left), std::move(right));
}

/// @brief Create a greater-than expression.
inline LogicalExprPtr gt(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Gt>(std::move(left), std::move(right));
}

/// @brief Create a greater-than-or-equal expression.
inline LogicalExprPtr gte(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<GtEq>(std::move(left), std::move(right));
}

/// @brief Create a less-than expression.
inline LogicalExprPtr lt(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Lt>(std::move(left), std::move(right));
}

/// @brief Create a less-than-or-equal expression.
inline LogicalExprPtr lte(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<LtEq>(std::move(left), std::move(right));
}

/// @brief Create a logical AND expression.
inline LogicalExprPtr and_expr(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<And>(std::move(left), std::move(right));
}

/// @brief Create a logical OR expression.
inline LogicalExprPtr or_expr(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Or>(std::move(left), std::move(right));
}

}  // namespace quest

#endif  // QUEST_LOGICAL_EXPR_HPP
