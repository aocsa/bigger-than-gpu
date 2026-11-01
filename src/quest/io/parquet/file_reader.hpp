// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"
#include "quest/io/parquet/metadata.hpp"

namespace quest::io::parquet {

using columnar::ColumnVector;
using columnar::RecordBatch;
using columnar::Schema;

// =============================================================================
// Constants
// =============================================================================

constexpr char kParquetMagic[] = "PAR1";
constexpr size_t kMagicSize = 4;
constexpr size_t kFooterSize = 8;  // footer length (4) + magic (4)

// =============================================================================
// File Handle
// =============================================================================

/// @brief A read-only file. `Read()` uses pread(2) and keeps no file position, so the threads
/// that read row groups in parallel can share one handle.
class FileHandle {
 public:
  [[nodiscard]] static Result<FileHandle> Open(const std::string& path);

  FileHandle() = default;
  FileHandle(FileHandle&& other) noexcept;
  FileHandle& operator=(FileHandle&& other) noexcept;
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;
  ~FileHandle();

  [[nodiscard]] size_t size() const noexcept { return file_size_; }

  [[nodiscard]] Result<std::vector<uint8_t>> Read(size_t offset,
                                                   size_t length) const;

 private:
  std::string path_;
  int fd_ = -1;
  size_t file_size_ = 0;
};

// =============================================================================
// Parquet File Reader
// =============================================================================

/// Example:
/// @code
///   auto reader_result = ParquetFileReader::Open("data.parquet");
///   if (!reader_result.has_value()) {
///     std::cerr << "Error: " << reader_result.error().message() << "\n";
///     return;
///   }
///   auto& reader = reader_result.value();
///
///   // Read all columns from first row group
///   auto batch_result = reader.ReadRowGroup(0);
///   if (batch_result.has_value()) {
///     auto& batch = batch_result.value();
///     // Process batch...
///   }
/// @endcode
class ParquetFileReader {
 public:
  [[nodiscard]] static Result<ParquetFileReader> Open(const std::string& path);

  ParquetFileReader() = default;
  ParquetFileReader(ParquetFileReader&&) = default;
  ParquetFileReader& operator=(ParquetFileReader&&) = default;

  // Non-copyable.
  ParquetFileReader(const ParquetFileReader&) = delete;
  ParquetFileReader& operator=(const ParquetFileReader&) = delete;

  // ===========================================================================
  // Accessors
  // ===========================================================================

  [[nodiscard]] const Schema& schema() const noexcept { return schema_; }

  [[nodiscard]] const FileMetadata& metadata() const noexcept {
    return metadata_;
  }

  [[nodiscard]] size_t num_row_groups() const noexcept {
    return metadata_.row_groups.size();
  }

  [[nodiscard]] int64_t num_rows() const noexcept { return metadata_.num_rows; }

  // ===========================================================================
  // Row Group Reading
  // ===========================================================================

  /// @param columns Column names to read; empty reads all.
  [[nodiscard]] Result<RecordBatch> ReadRowGroup(
      size_t row_group_index,
      const std::vector<std::string>& columns = {});

 private:
  // ===========================================================================
  // Private Methods
  // ===========================================================================

  [[nodiscard]] Status ValidateFileFormat();

  [[nodiscard]] Status ReadMetadata();

  [[nodiscard]] Result<std::vector<uint8_t>> ReadColumnChunk(
      const ColumnChunkMetadata& column_meta);

  FileHandle file_;
  FileMetadata metadata_;
  Schema schema_;
  std::vector<LeafType> leaves_;  ///< How to read each field of schema_.
};

}  // namespace quest::io::parquet
