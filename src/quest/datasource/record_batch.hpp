// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cassert>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "column_vector.hpp"
#include "schema.hpp"

namespace quest::datasource {

// Batch of data organized in columns (similar to Arrow RecordBatch)
class RecordBatch {
 public:
  RecordBatch(Schema schema, std::vector<std::shared_ptr<ColumnVector>> columns)
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

  std::shared_ptr<ColumnVector> column_ptr(size_t index) const {
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
  std::vector<std::shared_ptr<ColumnVector>> columns_;
};

}  // namespace quest::datasource
