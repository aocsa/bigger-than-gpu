// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/logical/logical_expr.hpp"

#include <optional>

#include "quest/plan/logical/logical_plan.hpp"

namespace quest::plan::logical {

namespace {

[[nodiscard]] bool IsNullLiteral(const LogicalExpr& expr) {
  const auto* lit = dynamic_cast<const Literal*>(&expr);
  return lit != nullptr && lit->value().is_null();
}

}  // namespace

Result<Field> Column::ToField(const LogicalPlan& input) const {
  for (const auto& field : input.schema().fields()) {
    if (field.name == name_) {
      return field;
    }
  }
  return Error::SchemaError("No column named '" + name_ + "' in schema");
}

Result<Field> ColumnIndex::ToField(const LogicalPlan& input) const {
  const auto& fields = input.schema().fields();
  if (index_ >= fields.size()) {
    return Error::OutOfRange("Column index out of range");
  }
  return fields[index_];
}

Result<Field> Literal::ToField(const LogicalPlan& /*input*/) const {
  return Field{"literal", value_.type()};
}

std::string Literal::ToString() const { return value_.ToSql(); }

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
  return BinaryResultType(op_, IsNullLiteral(*left_) ? rt : lt, IsNullLiteral(*right_) ? lt : rt)
      .transform([&](DataType type) {
        return Field{
            left_->ToString() + " " + std::string(plan::ToString(op_)) + " " + right_->ToString(),
            type};
      });
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
    return UnifyType(type, IsNullLiteral(result) ? std::nullopt
                                                 : std::optional<DataType>(field->data_type));
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

Result<Field> AggregateExpr::ToField(const LogicalPlan& input) const {
  if (kind_ == AggregateKind::kCount || kind_ == AggregateKind::kCountStar) {
    return Field{ToString(), DataType::kInt64};
  }
  if (kind_ == AggregateKind::kAvg) {
    return Field{ToString(), DataType::kDouble};
  }
  auto input_field = input_->ToField(input);
  if (!input_field) {
    return input_field;
  }
  const DataType type = input_field->data_type;
  if (kind_ != AggregateKind::kSum) {  // MIN and MAX keep the input type
    return Field{ToString(), type};
  }
  const auto sum = SumType(type);
  if (!sum) {
    return Error::TypeError("SUM needs a numeric input, got " + columnar::GetTypeName(type));
  }
  return Field{ToString(), *sum};
}

std::string AggregateExpr::ToString() const {
  return std::string(plan::ToString(kind_)) + "(" + (input_ ? input_->ToString() : "*") + ")";
}

}  // namespace quest::plan::logical
