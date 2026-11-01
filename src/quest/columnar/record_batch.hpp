// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cassert>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"

namespace quest::columnar {

// Batch of data organized in columns (similar to Arrow RecordBatch)
class RecordBatch {
 public:
  RecordBatch(Schema schema, std::vector<ColumnVectorPtr> columns)
      : schema_(std::move(schema)), columns_(std::move(columns)) {
    assert((columns_.empty() || schema_.num_fields() == columns_.size()) &&
           "schema field count doesn't match column count");
  }

  const Schema& schema() const { return schema_; }

  size_t num_rows() const {
    return columns_.empty() ? 0 : columns_[0]->size();
  }

  size_t num_columns() const { return columns_.size(); }

  const ColumnVector& column(size_t index) const {
    assert(index < columns_.size() && "column index out of range");
    return *columns_[index];
  }

  ColumnVectorPtr column_ptr(size_t index) const {
    assert(index < columns_.size() && "column index out of range");
    return columns_[index];
  }

  const ColumnVector* GetColumnByName(const std::string& name) const {
    auto idx = schema_.GetFieldIndex(name);
    if (!idx.has_value()) return nullptr;
    return columns_[*idx].get();
  }

  // Alias for compatibility with Kotlin API
  const ColumnVector& field(size_t index) const { return column(index); }

  std::string ToCSV() const {
    std::ostringstream oss;
    size_t rows = num_rows();
    size_t cols = num_columns();

    for (size_t row = 0; row < rows; ++row) {
      for (size_t col = 0; col < cols; ++col) {
        if (col > 0) oss << ",";
        Value val = columns_[col]->GetValue(row);
        oss << ValueToString(val);
      }
      oss << "\n";
    }
    return oss.str();
  }

  std::string ToString() const { return ToCSV(); }

 private:
  Schema schema_;
  std::vector<ColumnVectorPtr> columns_;
};

/// @brief Column-wise concatenate @p batches into one RecordBatch.
/// @param batches Non-null batches sharing the first batch's field count.
/// @return The merged batch, or SchemaError if field counts differ.
[[nodiscard]] inline Result<RecordBatch> ConcatenateRecordBatches(
    const std::vector<const RecordBatch*>& batches) {
  if (batches.empty()) {
    return Error::Invalid("No record batches to concatenate");
  }

  size_t total_rows = 0;
  for (const auto* batch : batches) {
    total_rows += batch->num_rows();
  }

  const auto& schema = batches[0]->schema();
  const size_t num_cols = schema.num_fields();
  for (size_t i = 1; i < batches.size(); ++i) {
    if (batches[i]->schema().num_fields() != num_cols) {
      return Error::SchemaError("Concatenate schema mismatch");
    }
  }

  std::vector<ColumnVectorPtr> columns;
  columns.reserve(num_cols);
  for (size_t c = 0; c < num_cols; ++c) {
    const DataType type = schema.field(c).data_type;
    auto column = VisitType(type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
      using T = typename Tag::CType;
      ColumnBuilder<T> builder(type, total_rows);
      for (const auto* batch : batches) {
        const ColumnVector& src = batch->column(c);
        if (src.GetType() != type) {
          return Error::Invalid("Concatenate column type mismatch");
        }
        const auto view = src.View<T>();
        for (size_t i = 0; i < src.size(); ++i) {
          view.IsNull(i) ? builder.AppendNull() : builder.Append(view[i]);
        }
      }
      return std::move(builder).Finish();
    });
    if (!column) {
      return std::unexpected(column.error());
    }
    columns.push_back(std::move(*column));
  }

  return RecordBatch(schema, std::move(columns));
}

}  // namespace quest::columnar
