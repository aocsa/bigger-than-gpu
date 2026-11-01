// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file parquet_data_source.hpp
/// @brief DataSource over a Parquet file; one partition per row group.

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "quest/common/result.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/io/parquet/file_reader.hpp"

namespace quest::datasource {

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
///   for (size_t i = 0; i < source.num_partitions(); ++i) {
///     auto batch_result = source.ReadPartition(i, {});
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

  [[nodiscard]] size_t num_partitions() const override { return reader_.num_row_groups(); }

  [[nodiscard]] Result<RecordBatch> ReadPartition(
      size_t index, const std::vector<std::string>& projection) override {
    return reader_.ReadRowGroup(index, projection);
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
