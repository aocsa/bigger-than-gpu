// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file logical_expr.hpp
/// @brief Logical expressions for query plans.

#include <memory>
#include <string>
#include <utility>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"

namespace quest::plan::logical {

class LogicalPlan;

using columnar::DataType;
using columnar::Field;
using columnar::Value;

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
  explicit Literal(Value value) : value_(std::move(value)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    DataType dtype = std::visit(
        [](const auto& v) -> DataType {
          using T = std::decay_t<decltype(v)>;
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

class BinaryExpr : public LogicalExpr {
 public:
  BinaryExpr(LogicalExprPtr left, LogicalExprPtr right, std::string op)
      : left_(std::move(left)), right_(std::move(right)), op_(std::move(op)) {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    return Field{left_->ToString() + " " + op_ + " " + right_->ToString(),
                 columnar::BoolType{}};
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
// Arithmetic Operators
// =============================================================================

/// @brief Arithmetic binary expression; result type matches the left operand.
class ArithmeticBinaryExpr : public BinaryExpr {
 public:
  using BinaryExpr::BinaryExpr;

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override {
    auto left_field = left_->ToField(input);
    if (!left_field.has_value()) {
      return left_field;
    }
    // Use the left operand's type for the result (could be improved with type promotion)
    return Field{left_->ToString() + " " + op_ + " " + right_->ToString(),
                 left_field.value().data_type};
  }
};

class Add : public ArithmeticBinaryExpr {
 public:
  Add(LogicalExprPtr left, LogicalExprPtr right)
      : ArithmeticBinaryExpr(std::move(left), std::move(right), "+") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Add>(left_->Clone(), right_->Clone());
  }
};

class Subtract : public ArithmeticBinaryExpr {
 public:
  Subtract(LogicalExprPtr left, LogicalExprPtr right)
      : ArithmeticBinaryExpr(std::move(left), std::move(right), "-") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Subtract>(left_->Clone(), right_->Clone());
  }
};

class Multiply : public ArithmeticBinaryExpr {
 public:
  Multiply(LogicalExprPtr left, LogicalExprPtr right)
      : ArithmeticBinaryExpr(std::move(left), std::move(right), "*") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Multiply>(left_->Clone(), right_->Clone());
  }
};

class Divide : public ArithmeticBinaryExpr {
 public:
  Divide(LogicalExprPtr left, LogicalExprPtr right)
      : ArithmeticBinaryExpr(std::move(left), std::move(right), "/") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Divide>(left_->Clone(), right_->Clone());
  }
};

class Modulo : public ArithmeticBinaryExpr {
 public:
  Modulo(LogicalExprPtr left, LogicalExprPtr right)
      : ArithmeticBinaryExpr(std::move(left), std::move(right), "%") {}

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Modulo>(left_->Clone(), right_->Clone());
  }
};

// =============================================================================
// Aggregate Expressions
// =============================================================================

class AggregateExpr : public LogicalExpr {
 public:
  explicit AggregateExpr(LogicalExprPtr input, std::string op)
      : input_(std::move(input)), op_(std::move(op)) {}

  [[nodiscard]] const LogicalExpr& input() const { return *input_; }
  [[nodiscard]] const std::string& op() const { return op_; }

 protected:
  LogicalExprPtr input_;
  std::string op_;
};

class Sum : public AggregateExpr {
 public:
  explicit Sum(LogicalExprPtr input)
      : AggregateExpr(std::move(input), "SUM") {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override {
    auto input_field = input_->ToField(input);
    if (!input_field.has_value()) {
      return input_field;
    }
    // SUM returns double for numeric types
    return Field{"SUM(" + input_->ToString() + ")", columnar::DoubleType{}};
  }

  [[nodiscard]] std::string ToString() const override {
    return "SUM(" + input_->ToString() + ")";
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Sum>(input_->Clone());
  }
};

class Avg : public AggregateExpr {
 public:
  explicit Avg(LogicalExprPtr input)
      : AggregateExpr(std::move(input), "AVG") {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    // AVG always returns double
    return Field{"AVG(" + input_->ToString() + ")", columnar::DoubleType{}};
  }

  [[nodiscard]] std::string ToString() const override {
    return "AVG(" + input_->ToString() + ")";
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Avg>(input_->Clone());
  }
};

class Count : public AggregateExpr {
 public:
  explicit Count(LogicalExprPtr input)
      : AggregateExpr(std::move(input), "COUNT") {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    // COUNT always returns Int64
    return Field{"COUNT(" + input_->ToString() + ")", columnar::Int64Type{}};
  }

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(" + input_->ToString() + ")";
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Count>(input_->Clone());
  }
};

class CountStar : public LogicalExpr {
 public:
  CountStar() = default;

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& /*input*/) const override {
    return Field{"COUNT(*)", columnar::Int64Type{}};
  }

  [[nodiscard]] std::string ToString() const override {
    return "COUNT(*)";
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<CountStar>();
  }
};

class Min : public AggregateExpr {
 public:
  explicit Min(LogicalExprPtr input)
      : AggregateExpr(std::move(input), "MIN") {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override {
    auto input_field = input_->ToField(input);
    if (!input_field.has_value()) {
      return input_field;
    }
    // MIN preserves input type
    return Field{"MIN(" + input_->ToString() + ")", input_field.value().data_type};
  }

  [[nodiscard]] std::string ToString() const override {
    return "MIN(" + input_->ToString() + ")";
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Min>(input_->Clone());
  }
};

class Max : public AggregateExpr {
 public:
  explicit Max(LogicalExprPtr input)
      : AggregateExpr(std::move(input), "MAX") {}

  [[nodiscard]] Result<Field> ToField(const LogicalPlan& input) const override {
    auto input_field = input_->ToField(input);
    if (!input_field.has_value()) {
      return input_field;
    }
    // MAX preserves input type
    return Field{"MAX(" + input_->ToString() + ")", input_field.value().data_type};
  }

  [[nodiscard]] std::string ToString() const override {
    return "MAX(" + input_->ToString() + ")";
  }

  [[nodiscard]] std::unique_ptr<LogicalExpr> Clone() const override {
    return std::make_unique<Max>(input_->Clone());
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

inline LogicalExprPtr col(const std::string& name) {
  return std::make_unique<Column>(name);
}

inline LogicalExprPtr col_idx(size_t index) {
  return std::make_unique<ColumnIndex>(index);
}

template <typename T>
LogicalExprPtr lit(T value) {
  return std::make_unique<Literal>(Value{value});
}

inline LogicalExprPtr eq(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Eq>(std::move(left), std::move(right));
}

inline LogicalExprPtr neq(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Neq>(std::move(left), std::move(right));
}

inline LogicalExprPtr gt(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Gt>(std::move(left), std::move(right));
}

inline LogicalExprPtr gte(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<GtEq>(std::move(left), std::move(right));
}

inline LogicalExprPtr lt(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Lt>(std::move(left), std::move(right));
}

inline LogicalExprPtr lte(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<LtEq>(std::move(left), std::move(right));
}

inline LogicalExprPtr and_expr(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<And>(std::move(left), std::move(right));
}

inline LogicalExprPtr or_expr(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Or>(std::move(left), std::move(right));
}

inline LogicalExprPtr add(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Add>(std::move(left), std::move(right));
}

inline LogicalExprPtr subtract(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Subtract>(std::move(left), std::move(right));
}

inline LogicalExprPtr multiply(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Multiply>(std::move(left), std::move(right));
}

inline LogicalExprPtr divide(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Divide>(std::move(left), std::move(right));
}

inline LogicalExprPtr modulo(LogicalExprPtr left, LogicalExprPtr right) {
  return std::make_unique<Modulo>(std::move(left), std::move(right));
}

// =============================================================================
// Aggregate Factory Functions
// =============================================================================

inline LogicalExprPtr sum(LogicalExprPtr input) {
  return std::make_unique<Sum>(std::move(input));
}

inline LogicalExprPtr avg(LogicalExprPtr input) {
  return std::make_unique<Avg>(std::move(input));
}

inline LogicalExprPtr count(LogicalExprPtr input) {
  return std::make_unique<Count>(std::move(input));
}

inline LogicalExprPtr count_star() {
  return std::make_unique<CountStar>();
}

inline LogicalExprPtr min(LogicalExprPtr input) {
  return std::make_unique<Min>(std::move(input));
}

inline LogicalExprPtr max(LogicalExprPtr input) {
  return std::make_unique<Max>(std::move(input));
}

}  // namespace quest::plan::logical
