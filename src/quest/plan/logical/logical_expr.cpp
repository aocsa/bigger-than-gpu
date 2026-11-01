// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/logical/logical_expr.hpp"

#include "quest/plan/logical/logical_plan.hpp"

namespace quest::plan::logical {

namespace {

[[nodiscard]] bool IsNullLiteral(const LogicalExpr& expr) {
  const auto* lit = dynamic_cast<const Literal*>(&expr);
  return lit != nullptr && std::holds_alternative<std::monostate>(lit->value());
}

}  // namespace

Result<Field> Column::ToField(const LogicalPlan& input) const {
  for (const auto& field : input.schema().fields()) {
    if (field.name == name_) {
      return field;
    }
  }
  return std::unexpected(
      Error::SchemaError("No column named '" + name_ + "' in schema"));
}

Result<Field> ColumnIndex::ToField(const LogicalPlan& input) const {
  const auto& fields = input.schema().fields();
  if (index_ >= fields.size()) {
    return std::unexpected(Error::OutOfRange("Column index out of range"));
  }
  return fields[index_];
}

Result<Field> Literal::ToField(const LogicalPlan& /*input*/) const {
  DataType dtype = std::visit(
      [](const auto& v) -> DataType {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, int8_t>) {
          return columnar::Int8Type{};
        } else if constexpr (std::is_same_v<T, int16_t>) {
          return columnar::Int16Type{};
        } else if constexpr (std::is_same_v<T, int32_t>) {
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

std::string Literal::ToString() const {
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

Result<Field> BinaryExpr::ToField(const LogicalPlan& input) const {
  auto left_field = left_->ToField(input);
  if (!left_field) {
    return left_field;
  }
  auto right_field = right_->ToField(input);
  if (!right_field) {
    return right_field;
  }
  const DataType& lt = left_field->data_type;
  const DataType& rt = right_field->data_type;
  auto type = BinaryResultType(op_, IsNullLiteral(*left_) ? rt : lt,
                               IsNullLiteral(*right_) ? lt : rt);
  if (!type) {
    return std::unexpected(type.error());
  }
  return Field{
      left_->ToString() + " " + std::string(plan::ToString(op_)) + " " + right_->ToString(),
      std::move(*type)};
}

std::string BinaryExpr::ToString() const {
  return "(" + left_->ToString() + " " + std::string(plan::ToString(op_)) + " " +
         right_->ToString() + ")";
}

Result<Field> Sum::ToField(const LogicalPlan& input) const {
  auto input_field = input_->ToField(input);
  if (!input_field.has_value()) {
    return input_field;
  }
  // SUM returns double for numeric types
  return Field{"SUM(" + input_->ToString() + ")", columnar::DoubleType{}};
}

Result<Field> Min::ToField(const LogicalPlan& input) const {
  auto input_field = input_->ToField(input);
  if (!input_field.has_value()) {
    return input_field;
  }
  // MIN preserves input type
  return Field{"MIN(" + input_->ToString() + ")", input_field.value().data_type};
}

Result<Field> Max::ToField(const LogicalPlan& input) const {
  auto input_field = input_->ToField(input);
  if (!input_field.has_value()) {
    return input_field;
  }
  // MAX preserves input type
  return Field{"MAX(" + input_->ToString() + ")", input_field.value().data_type};
}

}  // namespace quest::plan::logical
