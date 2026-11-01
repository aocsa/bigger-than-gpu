// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file batch_kernels.hpp
/// @brief Shared RecordBatch filter/project helpers for physical + taskflow paths.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/common/result.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"

namespace quest::execution {

/// @brief Gather selected rows from a column into a new materialized column.
[[nodiscard]] inline std::shared_ptr<columnar::ColumnVector> GatherColumn(
    const columnar::ColumnVector& col, const std::vector<size_t>& indices) {
  std::vector<columnar::Value> filtered_values;
  filtered_values.reserve(indices.size());
  for (size_t idx : indices) {
    filtered_values.push_back(col.GetValue(idx));
  }

  return std::visit(
      [&filtered_values](const auto& type_tag) -> std::shared_ptr<columnar::ColumnVector> {
        using TypeTag = std::decay_t<decltype(type_tag)>;

        if constexpr (std::is_same_v<TypeTag, columnar::BoolType>) {
          std::vector<bool> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<bool>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(false);
            }
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Int32Type>) {
          std::vector<int32_t> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<int32_t>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0);
            }
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Int64Type>) {
          std::vector<int64_t> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<int64_t>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0);
            }
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::FloatType>) {
          std::vector<float> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<float>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0.0f);
            }
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::DoubleType>) {
          std::vector<double> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<double>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0.0);
            }
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::StringType>) {
          std::vector<std::string> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<std::string>(&val)) {
              data.push_back(*v);
            } else {
              data.emplace_back();
            }
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, type_tag);
        } else {
          std::vector<std::string> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            data.push_back(columnar::ValueToString(val));
          }
          return std::make_shared<plan::physical::MaterializedColumnVector>(
              plan::physical::ColumnData{std::move(data)}, columnar::StringType{});
        }
      },
      col.GetType());
}

/// @brief Apply a boolean mask expression and gather matching rows.
[[nodiscard]] inline Result<columnar::RecordBatch> FilterRecordBatch(
    const columnar::RecordBatch& batch, plan::physical::Expression& expr) {
  auto mask_result = expr.Evaluate(batch);
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
    return columnar::RecordBatch(batch.schema(), {});
  }

  std::vector<std::shared_ptr<columnar::ColumnVector>> new_columns;
  new_columns.reserve(batch.num_columns());
  for (size_t col_idx = 0; col_idx < batch.num_columns(); ++col_idx) {
    new_columns.push_back(GatherColumn(batch.column(col_idx), indices));
  }
  return columnar::RecordBatch(batch.schema(), std::move(new_columns));
}

/// @brief Truncate a RecordBatch to its first @p n_rows rows via GatherColumn.
///
/// If @p n_rows is 0, returns an empty batch with the same schema. If
/// @p n_rows >= batch.num_rows(), gathers every row (a full copy).
///
/// @note Inherits GatherColumn's null handling: gathered values lose their
/// null bitmap (nulls become type defaults). A Limit whose boundary batch is
/// truncated therefore drops nulls in that batch while pass-through batches
/// keep them — resolved when materialized columns track nulls (phase 7).
[[nodiscard]] inline Result<columnar::RecordBatch> TruncateRecordBatch(
    const columnar::RecordBatch& batch, std::size_t n_rows) {
  if (n_rows == 0) {
    return columnar::RecordBatch(batch.schema(), {});
  }
  const std::size_t take = std::min(n_rows, batch.num_rows());
  std::vector<size_t> indices(take);
  for (std::size_t i = 0; i < take; ++i) {
    indices[i] = i;
  }

  std::vector<std::shared_ptr<columnar::ColumnVector>> new_columns;
  new_columns.reserve(batch.num_columns());
  for (size_t col_idx = 0; col_idx < batch.num_columns(); ++col_idx) {
    new_columns.push_back(GatherColumn(batch.column(col_idx), indices));
  }
  return columnar::RecordBatch(batch.schema(), std::move(new_columns));
}

/// @brief Evaluate projection expressions into a new RecordBatch.
[[nodiscard]] inline Result<columnar::RecordBatch> ProjectRecordBatch(
    const columnar::RecordBatch& batch, const columnar::Schema& schema,
    const std::vector<plan::physical::ExpressionPtr>& expressions) {
  std::vector<std::shared_ptr<columnar::ColumnVector>> columns;
  columns.reserve(expressions.size());
  for (const auto& expr : expressions) {
    auto result = expr->Evaluate(batch);
    if (!result) {
      return std::unexpected(result.error());
    }
    columns.push_back(std::move(*result));
  }
  if (columns.size() != expressions.size()) {
    return Error::ExpressionError("Projection produced fewer columns than expressions");
  }
  return columnar::RecordBatch(schema, std::move(columns));
}

}  // namespace quest::execution
