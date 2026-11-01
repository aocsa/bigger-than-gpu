// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/execution/kernels/batch_kernels.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace quest::execution {

columnar::ColumnVectorPtr GatherColumn(const columnar::ColumnVector& col,
                                       const std::vector<size_t>& indices) {
  return columnar::VisitType(col.GetType(), [&]<typename Tag>(Tag) {
    using T = typename Tag::CType;
    const auto view = col.View<T>();
    columnar::ColumnBuilder<T> builder(col.GetType(), indices.size());
    for (const size_t index : indices) {
      view.IsNull(index) ? builder.AppendNull() : builder.Append(view[index]);
    }
    return std::move(builder).Finish();
  });
}

Result<columnar::RecordBatch> FilterRecordBatch(
    const columnar::RecordBatch& batch, plan::physical::Expression& expr) {
  auto mask_result = expr.Evaluate(batch);
  if (!mask_result) {
    return std::unexpected(mask_result.error());
  }

  const auto& mask = **mask_result;
  std::vector<size_t> indices;
  if (mask.GetType() == columnar::DataType::kBool) {
    indices.reserve(batch.num_rows());
    const auto view = mask.View<bool>();
    for (size_t i = 0; i < batch.num_rows(); ++i) {
      if (!view.IsNull(i) && view[i]) {
        indices.push_back(i);
      }
    }
  } else if (mask.null_count() != mask.size()) {  // a NULL literal keeps no row
    return Error::TypeError("Filter needs a Bool predicate, got " +
                            columnar::GetTypeName(mask.GetType()));
  }

  if (indices.empty()) {
    return columnar::RecordBatch(batch.schema(), {});
  }

  std::vector<columnar::ColumnVectorPtr> new_columns;
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

  std::vector<columnar::ColumnVectorPtr> new_columns;
  new_columns.reserve(batch.num_columns());
  for (size_t col_idx = 0; col_idx < batch.num_columns(); ++col_idx) {
    new_columns.push_back(GatherColumn(batch.column(col_idx), indices));
  }
  return columnar::RecordBatch(batch.schema(), std::move(new_columns));
}

Result<columnar::RecordBatch> ProjectRecordBatch(
    const columnar::RecordBatch& batch, const columnar::Schema& schema,
    const std::vector<plan::physical::ExpressionPtr>& expressions) {
  std::vector<columnar::ColumnVectorPtr> columns;
  columns.reserve(expressions.size());
  for (const auto& expr : expressions) {
    auto result = expr->Evaluate(batch);
    if (!result) {
      return std::unexpected(result.error());
    }
    // A literal evaluates to a constant column; a batch holds only plain columns.
    if ((*result)->is_constant()) {
      std::vector<size_t> rows(batch.num_rows());
      std::iota(rows.begin(), rows.end(), size_t{0});
      result = GatherColumn(**result, rows);
    }
    columns.push_back(std::move(*result));
  }
  if (columns.size() != expressions.size()) {
    return Error::ExpressionError("Projection produced fewer columns than expressions");
  }
  return columnar::RecordBatch(schema, std::move(columns));
}

}  // namespace quest::execution
