// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file logical_plan.hpp
/// @brief Logical plan nodes for query execution.

#include <cstddef>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "quest/datasource/data_source.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/result.hpp"

namespace quest {

using datasource::DataSource;
using datasource::Schema;

// =============================================================================
// Abstract Base Class
// =============================================================================

/// @brief Abstract base class for logical plan nodes.
class LogicalPlan {
 public:
  virtual ~LogicalPlan() = default;

  [[nodiscard]] virtual Schema schema() const = 0;
  [[nodiscard]] virtual std::vector<const LogicalPlan*> Children() const = 0;
  [[nodiscard]] virtual std::string ToString() const = 0;

  /// @brief Pretty-prints the full plan subtree with indentation.
  [[nodiscard]] std::string Pretty(size_t indent = 0) const {
    std::string indentation(indent * 2, ' ');
    std::ostringstream result;
    result << indentation << ToString() << "\n";
    for (const auto* child : Children()) {
      result << child->Pretty(indent + 1);
    }
    return result.str();
  }
};

using LogicalPlanPtr = std::unique_ptr<LogicalPlan>;

// =============================================================================
// Scan Node
// =============================================================================

class Scan : public LogicalPlan {
 public:
  Scan(std::string path, std::shared_ptr<DataSource> data_source,
       std::optional<std::vector<std::string>> projection = std::nullopt)
      : path_(std::move(path)),
        data_source_(std::move(data_source)),
        projection_(std::move(projection)),
        schema_(DeriveSchema()) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {};
  }

  [[nodiscard]] std::string ToString() const override {
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

  [[nodiscard]] const std::string& path() const { return path_; }
  [[nodiscard]] const std::shared_ptr<DataSource>& data_source() const {
    return data_source_;
  }
  [[nodiscard]] const std::optional<std::vector<std::string>>& projection()
      const {
    return projection_;
  }

 private:
  [[nodiscard]] Schema DeriveSchema() const {
    const auto& source_schema = data_source_->schema();
    if (projection_) {
      return source_schema.Select(*projection_);
    }
    return source_schema;
  }

  std::string path_;
  std::shared_ptr<DataSource> data_source_;
  std::optional<std::vector<std::string>> projection_;
  Schema schema_;
};

// =============================================================================
// Selection Node
// =============================================================================

class Selection : public LogicalPlan {
 public:
  Selection(LogicalPlanPtr input, LogicalExprPtr condition)
      : input_(std::move(input)), condition_(std::move(condition)) {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
    return "Selection: " + condition_->ToString();
  }

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] const LogicalExpr& condition() const { return *condition_; }

 private:
  LogicalPlanPtr input_;
  LogicalExprPtr condition_;
};

// =============================================================================
// Projection Node
// =============================================================================

class Projection : public LogicalPlan {
 public:
  Projection(LogicalPlanPtr input, std::vector<LogicalExprPtr> expressions)
      : input_(std::move(input)), expressions_(std::move(expressions)) {}

  [[nodiscard]] Schema schema() const override {
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

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
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

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] const std::vector<LogicalExprPtr>& expressions() const {
    return expressions_;
  }

 private:
  LogicalPlanPtr input_;
  std::vector<LogicalExprPtr> expressions_;
};

// =============================================================================
// Limit Node
// =============================================================================

/// @brief Limit node - truncates the stream to the first @p limit rows.
class Limit : public LogicalPlan {
 public:
  Limit(LogicalPlanPtr input, std::size_t limit)
      : input_(std::move(input)), limit_(limit) {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] std::string ToString() const override {
    return "Limit: " + std::to_string(limit_);
  }

  [[nodiscard]] const LogicalPlan& input() const { return *input_; }
  [[nodiscard]] std::size_t limit() const noexcept { return limit_; }

 private:
  LogicalPlanPtr input_;
  std::size_t limit_;
};

// =============================================================================
// Union Node
// =============================================================================

/// @brief Union node - concatenates two input streams (UNION ALL semantics).
///
/// The first multi-child logical node. Both inputs must share a schema; the
/// gate/physical layer rejects a mismatch. Output schema is the left input's.
class Union : public LogicalPlan {
 public:
  Union(LogicalPlanPtr left, LogicalPlanPtr right)
      : left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] Schema schema() const override { return left_->schema(); }

  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override {
    return {left_.get(), right_.get()};
  }

  [[nodiscard]] std::string ToString() const override { return "Union All"; }

  [[nodiscard]] const LogicalPlan& left() const { return *left_; }
  [[nodiscard]] const LogicalPlan& right() const { return *right_; }

 private:
  LogicalPlanPtr left_;
  LogicalPlanPtr right_;
};

/// @brief Whether two schemas are UNION-compatible (same field count + types).
[[nodiscard]] inline bool UnionCompatible(const Schema& lhs, const Schema& rhs) {
  if (lhs.num_fields() != rhs.num_fields()) {
    return false;
  }
  for (std::size_t i = 0; i < lhs.num_fields(); ++i) {
    if (datasource::GetTypeName(lhs.field(i).data_type) !=
        datasource::GetTypeName(rhs.field(i).data_type)) {
      return false;
    }
  }
  return true;
}

// =============================================================================
// Column/ColumnIndex ToField Implementations
// =============================================================================

inline Result<Field> Column::ToField(const LogicalPlan& input) const {
  for (const auto& field : input.schema().fields()) {
    if (field.name == name_) {
      return field;
    }
  }
  return std::unexpected(
      Status::SchemaError("No column named '" + name_ + "' in schema"));
}

inline Result<Field> ColumnIndex::ToField(const LogicalPlan& input) const {
  const auto& fields = input.schema().fields();
  if (index_ >= fields.size()) {
    return std::unexpected(Status::OutOfRange("Column index out of range"));
  }
  return fields[index_];
}

}  // namespace quest
