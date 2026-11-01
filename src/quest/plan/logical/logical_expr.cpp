// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/logical/logical_expr.hpp"

#include <optional>

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
        } else if constexpr (std::is_same_v<T, columnar::Date32>) {
          return "DATE '" + columnar::ValueToString(v) + "'";
        } else if constexpr (std::is_same_v<T, columnar::Timestamp>) {
          return "TIMESTAMP '" + columnar::ValueToString(v) + "'";
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

Result<Field> CaseExpr::ToField(const LogicalPlan& input) const {
  std::optional<DataType> type;
  auto unify = [&](const LogicalExpr& result) -> Status {
    auto field = result.ToField(input);
    if (!field) {
      return std::unexpected(field.error());
    }
    if (IsNullLiteral(result)) {
      return {};
    }
    if (!type) {
      type = field->data_type;
      return {};
    }
    auto common = CommonType(*type, field->data_type);
    if (!common) {
      return std::unexpected(common.error());
    }
    type = std::move(*common);
    return {};
  };
  for (const auto& [when, then] : branches_) {
    auto condition = when->ToField(input);
    if (!condition) {
      return condition;
    }
    if (!IsNullLiteral(*when) &&
        !(condition->data_type == columnar::DataType::kBool)) {
      return Error::TypeError("CASE WHEN needs a bool condition, got " +
                              columnar::GetTypeName(condition->data_type));
    }
    if (auto status = unify(*then); !status) {
      return std::unexpected(status.error());
    }
  }
  if (auto status = unify(*otherwise_); !status) {
    return std::unexpected(status.error());
  }
  if (!type) {
    type = otherwise_->ToField(input)->data_type;  // every result is a NULL literal
  }
  return Field{ToString(), std::move(*type)};
}

std::string CaseExpr::ToString() const {
  std::string s = "CASE";
  for (const auto& [when, then] : branches_) {
    s += " WHEN " + when->ToString() + " THEN " + then->ToString();
  }
  return s + " ELSE " + otherwise_->ToString() + " END";
}

std::unique_ptr<LogicalExpr> CaseExpr::Clone() const {
  Branches branches;
  branches.reserve(branches_.size());
  for (const auto& [when, then] : branches_) {
    branches.emplace_back(when->Clone(), then->Clone());
  }
  return std::make_unique<CaseExpr>(std::move(branches), otherwise_->Clone());
}

Result<Field> Sum::ToField(const LogicalPlan& input) const {
  auto input_field = input_->ToField(input);
  if (!input_field.has_value()) {
    return input_field;
  }
  // SQL: SUM of an integer type is BIGINT, of a floating type DOUBLE.
  const auto& type = input_field->data_type;
  const bool is_integral = (type == columnar::DataType::kBool) ||
                           (type == columnar::DataType::kInt8) ||
                           (type == columnar::DataType::kInt16) ||
                           (type == columnar::DataType::kInt32) ||
                           (type == columnar::DataType::kInt64);
  const bool is_floating = (type == columnar::DataType::kFloat) ||
                           (type == columnar::DataType::kDouble);
  if (!is_integral && !is_floating) {
    return Error::TypeError("SUM needs a numeric input, got " + columnar::GetTypeName(type));
  }
  return Field{"SUM(" + input_->ToString() + ")",
               is_integral ? columnar::DataType{columnar::DataType::kInt64}
                           : columnar::DataType{columnar::DataType::kDouble}};
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
