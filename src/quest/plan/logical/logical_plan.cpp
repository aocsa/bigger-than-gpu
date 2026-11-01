// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/plan/logical/logical_plan.hpp"

#include <sstream>

namespace quest::plan::logical {

std::string LogicalPlan::Pretty(size_t indent) const {
  std::string indentation(indent * 2, ' ');
  std::ostringstream result;
  result << indentation << ToString() << "\n";
  for (const auto* child : Children()) {
    result << child->Pretty(indent + 1);
  }
  return result.str();
}

std::string Scan::ToString() const {
  std::ostringstream oss;
  oss << "Scan: " << path_ << "; projection=";
  if (projection_) {
    oss << "[";
    for (size_t i = 0; i < projection_->size(); ++i) {
      if (i > 0) {
        oss << ", ";
      }
      oss << (*projection_)[i];
    }
    oss << "]";
  } else {
    oss << "None";
  }
  return oss.str();
}

Result<std::unique_ptr<Scan>> Scan::WithProjection(std::string path,
                                                   std::shared_ptr<DataSource> data_source,
                                                   std::vector<std::string> columns) {
  auto schema = data_source->schema().Select(columns);
  if (!schema) {
    return std::unexpected(std::move(schema).error());
  }
  return std::make_unique<Scan>(Validated{}, std::move(path), std::move(data_source),
                                std::move(columns), *std::move(schema));
}

Schema Projection::schema() const {
  std::vector<Field> fields;
  fields.reserve(expressions_.size());
  for (const auto& expr : expressions_) {
    auto field_result = expr->ToField(*input_);
    if (field_result.has_value()) {
      fields.push_back(field_result.value());
    }
  }
  return Schema(std::move(fields));
}

std::string Projection::ToString() const {
  std::ostringstream oss;
  oss << "Projection: ";
  for (size_t i = 0; i < expressions_.size(); ++i) {
    if (i > 0) {
      oss << ", ";
    }
    oss << expressions_[i]->ToString();
  }
  return oss.str();
}

bool UnionCompatible(const Schema& lhs, const Schema& rhs) {
  if (lhs.num_fields() != rhs.num_fields()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.num_fields(); ++i) {
    if (columnar::GetTypeName(lhs.field(i).data_type) !=
        columnar::GetTypeName(rhs.field(i).data_type)) {
      return false;
    }
  }
  return true;
}

}  // namespace quest::plan::logical
