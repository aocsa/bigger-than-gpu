// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/execution/kernels/batch_kernels.hpp"

#include <algorithm>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace quest::execution {

namespace {

template <typename T>
[[nodiscard]] std::shared_ptr<columnar::ColumnVector> GatherTyped(
    const columnar::ColumnVector& col, const std::vector<size_t>& indices,
    columnar::DataType type) {
  std::vector<T> data;
  data.reserve(indices.size());
  std::vector<bool> validity;
  validity.reserve(indices.size());
  bool has_null = false;
  for (size_t idx : indices) {
    auto val = col.GetValue(idx);
    if (auto* v = std::get_if<T>(&val)) {
      data.push_back(*v);
      validity.push_back(true);
    } else if constexpr (std::is_same_v<T, std::string>) {
      // Binary and fixed-width byte types are carried as strings.
      if (std::holds_alternative<std::monostate>(val)) {
        data.emplace_back();
        validity.push_back(false);
        has_null = true;
      } else {
        data.push_back(columnar::ValueToString(val));
        validity.push_back(true);
      }
    } else {
      data.push_back(T{});
      validity.push_back(false);
      has_null = true;
    }
  }
  return std::make_shared<plan::physical::MaterializedColumnVector>(
      plan::physical::ColumnData{std::move(data)}, std::move(type),
      has_null ? std::move(validity) : std::vector<bool>{});
}

}  // namespace

std::shared_ptr<columnar::ColumnVector> GatherColumn(
    const columnar::ColumnVector& col, const std::vector<size_t>& indices) {
  return std::visit(
      [&](const auto& type_tag) -> std::shared_ptr<columnar::ColumnVector> {
        using TypeTag = std::decay_t<decltype(type_tag)>;
        if constexpr (std::is_same_v<TypeTag, columnar::BoolType>) {
          return GatherTyped<bool>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Int8Type>) {
          return GatherTyped<int8_t>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Int16Type>) {
          return GatherTyped<int16_t>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Int32Type>) {
          return GatherTyped<int32_t>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Int64Type>) {
          return GatherTyped<int64_t>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::FloatType>) {
          return GatherTyped<float>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::DoubleType>) {
          return GatherTyped<double>(col, indices, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, columnar::Date32Type>) {
          return GatherTyped<columnar::Date32>(col, indices, type_tag);
        } else {
          return GatherTyped<std::string>(col, indices, columnar::StringType{});
        }
      },
      col.GetType());
}

Result<columnar::RecordBatch> FilterRecordBatch(
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

Result<columnar::RecordBatch> TruncateRecordBatch(
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

Result<columnar::RecordBatch> ProjectRecordBatch(
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
