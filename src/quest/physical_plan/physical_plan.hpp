// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_PHYSICAL_PLAN_HPP
#define QUEST_PHYSICAL_PLAN_HPP

/// @file physical_plan.hpp
/// @brief Physical plan nodes for query execution.
///
/// This file defines the physical plan tree nodes that execute queries
/// on actual data sources.

#include <functional>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "quest/datasource/data_source.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/expressions/physical_expr.hpp"
#include "quest/result.hpp"

namespace quest {

// Use types from datasource namespace
using datasource::DataSource;
using datasource::RecordBatch;
using datasource::Schema;

/// @brief Callback for streaming batch results.
using ResultCallback = std::function<void(RecordBatch)>;

// =============================================================================
// Abstract Base Class
// =============================================================================

/// @brief Abstract base class for physical plan nodes.
class PhysicalPlan {
 public:
  virtual ~PhysicalPlan() = default;

  /// @brief Returns the output schema for this plan node.
  [[nodiscard]] virtual Schema schema() const = 0;

  /// @brief Executes the plan and calls the callback for each result batch.
  /// @param callback The callback to invoke for each batch.
  /// @return Status indicating success or failure.
  [[nodiscard]] virtual Status Execute(const ResultCallback& callback) = 0;

  /// @brief Returns the child plan nodes.
  [[nodiscard]] virtual std::vector<const PhysicalPlan*> Children() const = 0;

  /// @brief Returns a string representation of this node.
  [[nodiscard]] virtual std::string ToString() const = 0;

  /// @brief Returns a pretty-printed representation of the plan tree.
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

using PhysicalPlanPtr = std::unique_ptr<PhysicalPlan>;

// =============================================================================
// Scan Execution Node
// =============================================================================

/// @brief Scan execution node - reads from a data source.
class ScanExec : public PhysicalPlan {
 public:
  ScanExec(std::shared_ptr<DataSource> data_source,
           std::optional<std::vector<std::string>> projection)
      : data_source_(std::move(data_source)), projection_(std::move(projection)) {}

  [[nodiscard]] Schema schema() const override {
    if (projection_) {
      return data_source_->schema().Select(*projection_);
    }
    return data_source_->schema();
  }

  [[nodiscard]] std::vector<const PhysicalPlan*> Children() const override { return {}; }

  [[nodiscard]] Status Execute(const ResultCallback& callback) override {
    std::vector<std::string> proj;
    if (projection_) {
      proj = *projection_;
    } else {
      for (const auto& field : data_source_->schema().fields()) {
        proj.push_back(field.name);
      }
    }

    auto sequence = data_source_->Scan(proj);
    for (auto iter = sequence.begin(); iter != sequence.end(); ++iter) {
      auto batch_result = *iter;
      if (!batch_result) {
        return std::unexpected(batch_result.error());
      }
      callback(std::move(*batch_result));
    }
    return Status::OK();
  }

  [[nodiscard]] std::string ToString() const override {
    std::ostringstream oss;
    oss << "ScanExec(";
    if (projection_) {
      oss << "[";
      for (size_t i = 0; i < projection_->size(); ++i) {
        if (i > 0) {
          oss << ", ";
        }
        oss << (*projection_)[i];
      }
      oss << "]";
    }
    oss << ")";
    return oss.str();
  }

 private:
  std::shared_ptr<DataSource> data_source_;
  std::optional<std::vector<std::string>> projection_;
};

// =============================================================================
// Selection Execution Node
// =============================================================================

/// @brief Selection execution node - filters rows based on an expression.
class SelectionExec : public PhysicalPlan {
 public:
  SelectionExec(PhysicalPlanPtr input, ExpressionPtr expr)
      : input_(std::move(input)), expr_(std::move(expr)) {}

  [[nodiscard]] Schema schema() const override { return input_->schema(); }

  [[nodiscard]] std::vector<const PhysicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] Status Execute(const ResultCallback& callback) override {
    return input_->Execute([this, &callback](RecordBatch batch) {
      auto filtered_result = FilterBatch(batch);
      if (filtered_result.has_value() && filtered_result.value().num_rows() > 0) {
        callback(std::move(filtered_result.value()));
      }
    });
  }

  [[nodiscard]] std::string ToString() const override {
    return "SelectionExec(" + expr_->ToString() + ")";
  }

 private:
  [[nodiscard]] Result<RecordBatch> FilterBatch(const RecordBatch& batch) const {
    auto mask_result = expr_->Evaluate(batch);
    if (!mask_result) {
      return std::unexpected(mask_result.error());
    }

    auto& mask_col = **mask_result;
    std::vector<size_t> indices;
    indices.reserve(batch.num_rows());

    for (size_t i = 0; i < batch.num_rows(); ++i) {
      auto val = mask_col.GetValue(i);
      if (auto* bool_val = std::get_if<bool>(&val)) {
        if (*bool_val) {
          indices.push_back(i);
        }
      }
    }

    if (indices.empty()) {
      return RecordBatch(batch.schema(), {});
    }

    std::vector<std::shared_ptr<datasource::ColumnVector>> new_columns;
    new_columns.reserve(batch.num_columns());

    for (size_t col_idx = 0; col_idx < batch.num_columns(); ++col_idx) {
      auto& col = batch.column(col_idx);
      auto filtered = FilterColumn(col, indices);
      new_columns.push_back(std::move(filtered));
    }

    return RecordBatch(batch.schema(), std::move(new_columns));
  }

  [[nodiscard]] std::shared_ptr<datasource::ColumnVector> FilterColumn(
      const datasource::ColumnVector& col, const std::vector<size_t>& indices) const {
    std::vector<Value> filtered_values;
    filtered_values.reserve(indices.size());
    for (size_t idx : indices) {
      filtered_values.push_back(col.GetValue(idx));
    }

    // Create a materialized column with the filtered values
    // Determine the type and create appropriate vector
    return std::visit(
        [&filtered_values](const auto& type_tag) -> std::shared_ptr<datasource::ColumnVector> {
          using TypeTag = std::decay_t<decltype(type_tag)>;

          if constexpr (std::is_same_v<TypeTag, datasource::BoolType>) {
            std::vector<bool> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<bool>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(false);
              }
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::Int32Type>) {
            std::vector<int32_t> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<int32_t>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0);
              }
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::Int64Type>) {
            std::vector<int64_t> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<int64_t>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0);
              }
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::FloatType>) {
            std::vector<float> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<float>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0.0f);
              }
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::DoubleType>) {
            std::vector<double> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<double>(&val)) {
                data.push_back(*v);
              } else {
                data.push_back(0.0);
              }
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              type_tag);
          } else if constexpr (std::is_same_v<TypeTag, datasource::StringType>) {
            std::vector<std::string> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              if (auto* v = std::get_if<std::string>(&val)) {
                data.push_back(*v);
              } else {
                data.emplace_back();
              }
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              type_tag);
          } else {
            // Default: create string column
            std::vector<std::string> data;
            data.reserve(filtered_values.size());
            for (const auto& val : filtered_values) {
              data.push_back(datasource::ValueToString(val));
            }
            return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                              datasource::StringType{});
          }
        },
        col.GetType());
  }

  PhysicalPlanPtr input_;
  ExpressionPtr expr_;
};

// =============================================================================
// Projection Execution Node
// =============================================================================

/// @brief Projection execution node - selects/transforms columns.
class ProjectionExec : public PhysicalPlan {
 public:
  ProjectionExec(PhysicalPlanPtr input, Schema schema, std::vector<ExpressionPtr> expressions)
      : input_(std::move(input)),
        schema_(std::move(schema)),
        expressions_(std::move(expressions)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] std::vector<const PhysicalPlan*> Children() const override {
    return {input_.get()};
  }

  [[nodiscard]] Status Execute(const ResultCallback& callback) override {
    return input_->Execute([this, &callback](RecordBatch batch) {
      std::vector<std::shared_ptr<datasource::ColumnVector>> columns;
      columns.reserve(expressions_.size());
      for (const auto& expr : expressions_) {
        auto result = expr->Evaluate(batch);
        if (result.has_value()) {
          columns.push_back(std::move(result.value()));
        }
      }
      if (columns.size() == expressions_.size()) {
        callback(RecordBatch(schema_, std::move(columns)));
      }
    });
  }

  [[nodiscard]] std::string ToString() const override {
    std::ostringstream oss;
    oss << "ProjectionExec([";
    for (size_t i = 0; i < expressions_.size(); ++i) {
      if (i > 0) {
        oss << ", ";
      }
      oss << expressions_[i]->ToString();
    }
    oss << "])";
    return oss.str();
  }

 private:
  PhysicalPlanPtr input_;
  Schema schema_;
  std::vector<ExpressionPtr> expressions_;
};

// =============================================================================
// Physical Plan Factory Functions
// =============================================================================

/// @brief Creates a physical expression from a logical expression.
/// @param expr The logical expression.
/// @param input The input logical plan (for schema context).
/// @return The physical expression, or error.
[[nodiscard]] inline Result<ExpressionPtr> CreatePhysicalExpr(const LogicalExpr& expr,
                                                              const LogicalPlan& input) {
  if (const auto* literal = dynamic_cast<const Literal*>(&expr)) {
    return std::make_unique<LiteralExpression>(literal->value());
  }

  if (const auto* col_idx = dynamic_cast<const ColumnIndex*>(&expr)) {
    return std::make_unique<ColumnExpression>(col_idx->index());
  }

  if (const auto* col = dynamic_cast<const Column*>(&expr)) {
    auto index = input.schema().GetFieldIndex(col->name());
    if (!index) {
      return std::unexpected(Status::SchemaError("No column named: " + col->name()));
    }
    return std::make_unique<ColumnExpression>(*index);
  }

  if (const auto* binary = dynamic_cast<const BinaryExpr*>(&expr)) {
    auto left_result = CreatePhysicalExpr(binary->left(), input);
    if (!left_result) {
      return std::unexpected(left_result.error());
    }
    auto right_result = CreatePhysicalExpr(binary->right(), input);
    if (!right_result) {
      return std::unexpected(right_result.error());
    }

    auto left = std::move(*left_result);
    auto right = std::move(*right_result);

    if (dynamic_cast<const Eq*>(binary)) {
      return std::make_unique<EqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Neq*>(binary)) {
      return std::make_unique<NeqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Lt*>(binary)) {
      return std::make_unique<LtExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const LtEq*>(binary)) {
      return std::make_unique<LtEqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Gt*>(binary)) {
      return std::make_unique<GtExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const GtEq*>(binary)) {
      return std::make_unique<GtEqExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const And*>(binary)) {
      return std::make_unique<AndExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Or*>(binary)) {
      return std::make_unique<OrExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Add*>(binary)) {
      return std::make_unique<AddExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Subtract*>(binary)) {
      return std::make_unique<SubtractExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Multiply*>(binary)) {
      return std::make_unique<MultiplyExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Divide*>(binary)) {
      return std::make_unique<DivideExpression>(std::move(left), std::move(right));
    }
    if (dynamic_cast<const Modulo*>(binary)) {
      return std::make_unique<ModuloExpression>(std::move(left), std::move(right));
    }
  }

  return std::unexpected(
      Status::PhysicalPlanError("Cannot create physical expression from: " + expr.ToString()));
}

/// @brief Creates a physical plan from a logical plan.
/// @param plan The logical plan.
/// @return The physical plan, or error.
[[nodiscard]] inline Result<PhysicalPlanPtr> CreatePhysicalPlan(const LogicalPlan& plan) {
  if (const auto* scan = dynamic_cast<const Scan*>(&plan)) {
    return std::make_unique<ScanExec>(scan->data_source(), scan->projection());
  }

  if (const auto* selection = dynamic_cast<const Selection*>(&plan)) {
    auto input_result = CreatePhysicalPlan(selection->input());
    if (!input_result) {
      return std::unexpected(input_result.error());
    }
    auto filter_result = CreatePhysicalExpr(selection->condition(), selection->input());
    if (!filter_result) {
      return std::unexpected(filter_result.error());
    }
    return std::make_unique<SelectionExec>(std::move(*input_result), std::move(*filter_result));
  }

  if (const auto* projection = dynamic_cast<const Projection*>(&plan)) {
    auto input_result = CreatePhysicalPlan(projection->input());
    if (!input_result) {
      return std::unexpected(input_result.error());
    }

    std::vector<ExpressionPtr> exprs;
    exprs.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto expr_result = CreatePhysicalExpr(*expr, projection->input());
      if (!expr_result) {
        return std::unexpected(expr_result.error());
      }
      exprs.push_back(std::move(*expr_result));
    }

    std::vector<Field> fields;
    fields.reserve(projection->expressions().size());
    for (const auto& expr : projection->expressions()) {
      auto field_result = expr->ToField(projection->input());
      if (!field_result) {
        return std::unexpected(field_result.error());
      }
      fields.push_back(*field_result);
    }

    return std::make_unique<ProjectionExec>(std::move(*input_result), Schema(std::move(fields)),
                                            std::move(exprs));
  }

  return std::unexpected(Status::PhysicalPlanError("Unsupported logical plan type"));
}

}  // namespace quest

#endif  // QUEST_PHYSICAL_PLAN_HPP
