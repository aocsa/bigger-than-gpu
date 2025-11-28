// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_DATA_SOURCE_HPP
#define PARQUET_READER_DATA_SOURCE_HPP

/// @file data_source.hpp
/// @brief Abstract data source interface and Parquet implementation.
///
/// This file provides the DataSource interface for reading tabular data
/// and the ParquetDataSource implementation for Parquet files.

#include <memory>
#include <string>
#include <vector>

#include "file_reader.hpp"
#include "quest/result.hpp"
#include "record_batch.hpp"
#include "schema.hpp"

namespace quest::datasource {

// Forward declaration
class ParquetDataSource;

// =============================================================================
// Record Batch Iterator
// =============================================================================

/// @brief Iterator for scanning record batches from a Parquet file.
///
/// This iterator returns Result<RecordBatch> to handle errors during scanning.
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

  /// @brief Reads the current row group.
  /// @return The RecordBatch on success, or error Status.
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

/// @brief Range object for iterating over record batches.
///
/// This provides the begin/end interface for range-based for loops.
/// Note: Since dereferencing returns Result<RecordBatch>, users must
/// handle errors during iteration.
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

/// @brief Abstract data source interface.
///
/// This interface represents a source of tabular data that can be scanned
/// to produce record batches.
class DataSource {
 public:
  virtual ~DataSource() = default;

  /// @brief Returns the schema for the underlying data source.
  [[nodiscard]] virtual Schema schema() const = 0;

  /// @brief Scans the data source, selecting the specified columns.
  /// @param projection The columns to select (empty = all columns).
  /// @return A sequence that can be iterated to yield RecordBatch results.
  [[nodiscard]] virtual RecordBatchSequence Scan(
      const std::vector<std::string>& projection) = 0;
};

// =============================================================================
// Parquet Data Source
// =============================================================================

/// @brief Parquet file data source implementation.
///
/// This class provides a DataSource interface for reading Parquet files.
/// It must be opened using the static Open() factory method which returns
/// a Result to handle file opening errors.
///
/// Example usage:
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
  /// @brief Opens a Parquet file as a data source.
  /// @param filename The path to the Parquet file.
  /// @return The ParquetDataSource on success, or error Status.
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

  /// @brief Returns the Parquet file metadata.
  [[nodiscard]] const FileMetadata& metadata() const {
    return reader_.metadata();
  }

  /// @brief Returns the number of row groups in the file.
  [[nodiscard]] size_t num_row_groups() const {
    return reader_.num_row_groups();
  }

  /// @brief Returns the total number of rows in the file.
  [[nodiscard]] int64_t num_rows() const { return reader_.num_rows(); }

 private:
  ParquetDataSource(std::string filename, ParquetFileReader reader)
      : filename_(std::move(filename)), reader_(std::move(reader)) {}

  std::string filename_;
  ParquetFileReader reader_;
};

}  // namespace quest::datasource

#endif  // PARQUET_READER_DATA_SOURCE_HPP
