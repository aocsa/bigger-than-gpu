// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file batch_kernels.hpp
/// @brief Shared RecordBatch filter/project helpers for physical + taskflow paths.

#include <cstddef>
#include <memory>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/common/result.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"

namespace quest::execution {

/// @brief Gather selected rows from a column into a new materialized column, keeping nulls.
[[nodiscard]] columnar::ColumnVectorPtr GatherColumn(
    const columnar::ColumnVector& col, const std::vector<size_t>& indices);

/// @brief Apply a boolean mask expression and gather the rows where it is true; a null
/// mask value does not select its row.
[[nodiscard]] Result<columnar::RecordBatch> FilterRecordBatch(
    const columnar::RecordBatch& batch, plan::physical::Expression& expr);

/// @brief Truncate a RecordBatch to its first @p n_rows rows via GatherColumn.
///
/// If @p n_rows is 0, returns an empty batch with the same schema. If
/// @p n_rows >= batch.num_rows(), gathers every row (a full copy).
[[nodiscard]] Result<columnar::RecordBatch> TruncateRecordBatch(
    const columnar::RecordBatch& batch, std::size_t n_rows);

/// @brief Evaluate projection expressions into a new RecordBatch.
[[nodiscard]] Result<columnar::RecordBatch> ProjectRecordBatch(
    const columnar::RecordBatch& batch, const columnar::Schema& schema,
    const std::vector<plan::physical::ExpressionPtr>& expressions);

}  // namespace quest::execution
