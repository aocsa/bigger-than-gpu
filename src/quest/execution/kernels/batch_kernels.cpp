// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/execution/kernels/batch_kernels.hpp"

#include <algorithm>
#include <numeric>
#include <string>
#include <utility>

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

std::vector<columnar::ColumnVectorPtr> GatherColumns(const columnar::RecordBatch& batch,
                                                     const std::vector<size_t>& indices) {
  std::vector<columnar::ColumnVectorPtr> columns;
  columns.reserve(batch.num_columns());
  for (size_t col = 0; col < batch.num_columns(); ++col) {
    columns.push_back(GatherColumn(batch.column(col), indices));
  }
  return columns;
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
  return columnar::RecordBatch(batch.schema(), GatherColumns(batch, indices));
}

Result<columnar::RecordBatch> TruncateRecordBatch(
    const columnar::RecordBatch& batch, std::size_t n_rows) {
  if (n_rows == 0) {
    return columnar::RecordBatch(batch.schema(), {});
  }
  std::vector<size_t> indices(std::min(n_rows, batch.num_rows()));
  std::iota(indices.begin(), indices.end(), size_t{0});
  return columnar::RecordBatch(batch.schema(), GatherColumns(batch, indices));
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
  return columnar::RecordBatch(schema, std::move(columns));
}

}  // namespace quest::execution
