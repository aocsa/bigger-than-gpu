// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "file_reader.hpp"
#include "quest/result.hpp"
#include "record_batch.hpp"
#include "schema.hpp"

namespace quest::datasource {

class ParquetDataSource;

// =============================================================================
// Record Batch Iterator
// =============================================================================

/// Dereferencing yields Result<RecordBatch> so scan errors propagate without exceptions.
class RecordBatchIterator {
 public:
  RecordBatchIterator(ParquetFileReader* reader,
                      std::vector<std::string> columns, size_t index)
      : reader_(reader), columns_(std::move(columns)), index_(index) {}

  RecordBatchIterator& operator++() {
    ++index_;
    return *this;
  }

  bool operator!=(const RecordBatchIterator& other) const {
    return index_ != other.index_;
  }

  bool operator==(const RecordBatchIterator& other) const {
    return index_ == other.index_;
  }

  [[nodiscard]] Result<RecordBatch> operator*() const {
    return reader_->ReadRowGroup(index_, columns_);
  }

 private:
  ParquetFileReader* reader_;
  std::vector<std::string> columns_;
  size_t index_;
};

// =============================================================================
// Record Batch Sequence
// =============================================================================

class RecordBatchSequence {
 public:
  RecordBatchSequence(ParquetFileReader* reader,
                      std::vector<std::string> columns)
      : reader_(reader), columns_(std::move(columns)) {}

  [[nodiscard]] RecordBatchIterator begin() const {
    return RecordBatchIterator(reader_, columns_, 0);
  }

  [[nodiscard]] RecordBatchIterator end() const {
    return RecordBatchIterator(reader_, columns_, reader_->num_row_groups());
  }

 private:
  ParquetFileReader* reader_;
  std::vector<std::string> columns_;
};

// =============================================================================
// Abstract Data Source
// =============================================================================

class DataSource {
 public:
  virtual ~DataSource() = default;

  [[nodiscard]] virtual Schema schema() const = 0;

  /// @param projection Columns to select; empty selects all.
  [[nodiscard]] virtual RecordBatchSequence Scan(
      const std::vector<std::string>& projection) = 0;
};

// =============================================================================
// Parquet Data Source
// =============================================================================

/// Must be opened via Open() which returns Result to propagate file errors.
///
/// Example:
/// @code
///   auto source_result = ParquetDataSource::Open("data.parquet");
///   if (!source_result.has_value()) {
///     std::cerr << "Error: " << source_result.error().message() << "\n";
///     return;
///   }
///   auto& source = source_result.value();
///
///   for (auto it = source.Scan({}).begin(); it != source.Scan({}).end(); ++it) {
///     auto batch_result = *it;
///     if (batch_result.has_value()) {
///       // Process batch...
///     }
///   }
/// @endcode
class ParquetDataSource : public DataSource {
 public:
  [[nodiscard]] static Result<ParquetDataSource> Open(
      const std::string& filename) {
    auto reader_result = ParquetFileReader::Open(filename);
    if (!reader_result.has_value()) {
      return std::unexpected(reader_result.error());
    }
    return ParquetDataSource(filename, std::move(reader_result.value()));
  }

  ParquetDataSource(ParquetDataSource&&) = default;
  ParquetDataSource& operator=(ParquetDataSource&&) = default;

  // Non-copyable.
  ParquetDataSource(const ParquetDataSource&) = delete;
  ParquetDataSource& operator=(const ParquetDataSource&) = delete;

  [[nodiscard]] Schema schema() const override { return reader_.schema(); }

  [[nodiscard]] RecordBatchSequence Scan(
      const std::vector<std::string>& projection) override {
    return RecordBatchSequence(&reader_, projection);
  }

  // ===========================================================================
  // Parquet-Specific Accessors
  // ===========================================================================

  [[nodiscard]] const FileMetadata& metadata() const {
    return reader_.metadata();
  }

  [[nodiscard]] size_t num_row_groups() const {
    return reader_.num_row_groups();
  }

  [[nodiscard]] int64_t num_rows() const { return reader_.num_rows(); }

 private:
  ParquetDataSource(std::string filename, ParquetFileReader reader)
      : filename_(std::move(filename)), reader_(std::move(reader)) {}

  std::string filename_;
  ParquetFileReader reader_;
};

}  // namespace quest::datasource
