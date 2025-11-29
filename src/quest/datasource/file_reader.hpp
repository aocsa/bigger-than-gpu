// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_FILE_READER_HPP
#define PARQUET_READER_FILE_READER_HPP

/// @file file_reader.hpp
/// @brief Main Parquet file reader implementation.
///
/// This file provides the FileHandle and ParquetFileReader classes for
/// reading Parquet files. All operations return Result types for safe
/// error handling.

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "column_reader.hpp"
#include "compression.hpp"
#include "metadata.hpp"
#include "quest/result.hpp"
#include "record_batch.hpp"
#include "schema.hpp"

namespace quest::datasource {

// =============================================================================
// Constants
// =============================================================================

/// @brief Magic bytes at start and end of Parquet files.
constexpr char kParquetMagic[] = "PAR1";
constexpr size_t kMagicSize = 4;
constexpr size_t kFooterSize = 8;  // footer length (4) + magic (4)

// =============================================================================
// File Handle
// =============================================================================

/// @brief File handle for reading binary data from a file.
///
/// This class provides buffered reading from a file with error handling
/// via Result types.
class FileHandle {
 public:
  /// @brief Opens a file for reading.
  /// @param path The path to the file.
  /// @return A FileHandle on success, or IOError on failure.
  [[nodiscard]] static Result<FileHandle> Open(const std::string& path) {
    FileHandle handle;
    handle.path_ = path;
    handle.stream_.open(path, std::ios::binary | std::ios::ate);
    if (!handle.stream_) {
      return Status::IOError("Failed to open file: " + path);
    }
    handle.file_size_ = handle.stream_.tellg();
    handle.stream_.seekg(0);
    return handle;
  }

  FileHandle() = default;
  FileHandle(FileHandle&&) = default;
  FileHandle& operator=(FileHandle&&) = default;

  // Non-copyable due to ifstream.
  FileHandle(const FileHandle&) = delete;
  FileHandle& operator=(const FileHandle&) = delete;

  /// @brief Returns the file size in bytes.
  [[nodiscard]] size_t size() const noexcept { return file_size_; }

  /// @brief Reads bytes from the file into a buffer.
  /// @param offset The byte offset to start reading from.
  /// @param length The number of bytes to read.
  /// @param buffer The output buffer (will be resized).
  /// @return Status indicating success or IOError.
  [[nodiscard]] Status Read(size_t offset, size_t length,
                            std::vector<uint8_t>& buffer) {
    buffer.resize(length);
    stream_.seekg(static_cast<std::streamoff>(offset));
    stream_.read(reinterpret_cast<char*>(buffer.data()),
                 static_cast<std::streamsize>(length));
    if (!stream_) {
      return Status::IOError("Failed to read from file");
    }
    return Status::OK();
  }

  /// @brief Reads bytes from the file and returns them.
  /// @param offset The byte offset to start reading from.
  /// @param length The number of bytes to read.
  /// @return The bytes read, or IOError on failure.
  [[nodiscard]] Result<std::vector<uint8_t>> Read(size_t offset,
                                                   size_t length) {
    std::vector<uint8_t> buffer;
    auto status = Read(offset, length, buffer);
    if (!status.ok()) {
      return std::unexpected(status.error());
    }
    return buffer;
  }

 private:
  std::string path_;
  std::ifstream stream_;
  size_t file_size_ = 0;
};

// =============================================================================
// Parquet File Reader
// =============================================================================

/// @brief Main Parquet file reader.
///
/// This class provides the primary interface for reading Parquet files.
/// It handles file format validation, metadata parsing, and row group
/// reading with optional column projection.
///
/// Example usage:
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
  /// @brief Opens a Parquet file for reading.
  /// @param path The path to the Parquet file.
  /// @return A ParquetFileReader on success, or error Status on failure.
  [[nodiscard]] static Result<ParquetFileReader> Open(const std::string& path) {
    ParquetFileReader reader;

    // Open the file.
    auto file_result = FileHandle::Open(path);
    if (!file_result.has_value()) {
      return std::unexpected(file_result.error());
    }
    reader.file_ = std::move(file_result.value());

    // Validate format and read metadata.
    auto validate_status = reader.ValidateFileFormat();
    if (!validate_status.ok()) {
      return std::unexpected(validate_status.error());
    }

    auto metadata_status = reader.ReadMetadata();
    if (!metadata_status.ok()) {
      return std::unexpected(metadata_status.error());
    }

    return reader;
  }

  ParquetFileReader() = default;
  ParquetFileReader(ParquetFileReader&&) = default;
  ParquetFileReader& operator=(ParquetFileReader&&) = default;

  // Non-copyable.
  ParquetFileReader(const ParquetFileReader&) = delete;
  ParquetFileReader& operator=(const ParquetFileReader&) = delete;

  // ===========================================================================
  // Accessors
  // ===========================================================================

  /// @brief Returns the schema of the file.
  [[nodiscard]] const Schema& schema() const noexcept { return schema_; }

  /// @brief Returns the file metadata.
  [[nodiscard]] const FileMetadata& metadata() const noexcept {
    return metadata_;
  }

  /// @brief Returns the number of row groups in the file.
  [[nodiscard]] size_t num_row_groups() const noexcept {
    return metadata_.row_groups.size();
  }

  /// @brief Returns the total number of rows in the file.
  [[nodiscard]] int64_t num_rows() const noexcept { return metadata_.num_rows; }

  // ===========================================================================
  // Row Group Reading
  // ===========================================================================

  /// @brief Reads a specific row group.
  /// @param row_group_index The index of the row group to read.
  /// @param columns Optional list of column names to read (empty = all).
  /// @return The RecordBatch, or error Status on failure.
  [[nodiscard]] Result<RecordBatch> ReadRowGroup(
      size_t row_group_index,
      const std::vector<std::string>& columns = {}) {
    if (row_group_index >= metadata_.row_groups.size()) {
      return std::unexpected(
          Status::OutOfRange("Row group index out of range"));
    }

    const auto& row_group = metadata_.row_groups[row_group_index];

    // Determine which columns to read.
    std::vector<size_t> column_indices;
    Schema projected_schema;

    if (columns.empty()) {
      // Read all columns.
      for (size_t i = 0; i < schema_.num_fields(); ++i) {
        column_indices.push_back(i);
      }
      projected_schema = schema_;
    } else {
      // Read only specified columns.
      for (const auto& name : columns) {
        auto idx = schema_.GetFieldIndex(name);
        if (!idx.has_value()) {
          return std::unexpected(
              Status::NotFound("Column not found: " + name));
        }
        column_indices.push_back(*idx);
      }
      projected_schema = schema_.Select(columns);
    }

    // Read each column.
    std::vector<std::shared_ptr<ColumnVector>> column_vectors;
    column_vectors.reserve(column_indices.size());

    for (size_t col_idx : column_indices) {
      const auto& column_meta = row_group.columns[col_idx];

      auto column_data_result = ReadColumnChunk(column_meta);
      if (!column_data_result.has_value()) {
        return std::unexpected(column_data_result.error());
      }

      // Create compression codec for this column.
      auto codec_result = CreateCodec(column_meta.codec);
      if (!codec_result.has_value()) {
        return std::unexpected(codec_result.error());
      }

      ColumnReader reader(schema_.field(col_idx), column_meta,
                          column_data_result.value(),
                          std::move(codec_result.value()));
      auto column_result = reader.ReadAll();
      if (!column_result.has_value()) {
        return std::unexpected(column_result.error());
      }
      column_vectors.push_back(std::move(column_result.value()));
    }

    return RecordBatch(std::move(projected_schema), std::move(column_vectors));
  }

  // ===========================================================================
  // Row Group Iteration
  // ===========================================================================

  /// @brief Iterator for reading row groups.
  class RowGroupIterator {
   public:
    RowGroupIterator(ParquetFileReader* reader,
                     std::vector<std::string> columns, size_t index)
        : reader_(reader), columns_(std::move(columns)), index_(index) {}

    bool operator!=(const RowGroupIterator& other) const {
      return index_ != other.index_;
    }

    RowGroupIterator& operator++() {
      ++index_;
      return *this;
    }

    /// @brief Reads the current row group.
    /// @return The RecordBatch result.
    Result<RecordBatch> operator*() {
      return reader_->ReadRowGroup(index_, columns_);
    }

   private:
    ParquetFileReader* reader_;
    std::vector<std::string> columns_;
    size_t index_;
  };

  /// @brief Range for iterating over row groups.
  class RowGroupRange {
   public:
    RowGroupRange(ParquetFileReader* reader, std::vector<std::string> columns)
        : reader_(reader), columns_(std::move(columns)) {}

    RowGroupIterator begin() {
      return RowGroupIterator(reader_, columns_, 0);
    }

    RowGroupIterator end() {
      return RowGroupIterator(reader_, columns_, reader_->num_row_groups());
    }

   private:
    ParquetFileReader* reader_;
    std::vector<std::string> columns_;
  };

  /// @brief Returns a range for iterating over row groups.
  /// @param columns Optional list of column names to read.
  [[nodiscard]] RowGroupRange ScanRowGroups(
      const std::vector<std::string>& columns = {}) {
    return RowGroupRange(this, columns);
  }

 private:
  // ===========================================================================
  // Private Methods
  // ===========================================================================

  [[nodiscard]] Status ValidateFileFormat() {
    if (file_.size() < kMagicSize * 2 + 4) {
      return Status::ParquetFormatError(
          "File too small to be valid Parquet");
    }

    // Check magic at start.
    auto start_magic_result = file_.Read(0, kMagicSize);
    if (!start_magic_result.has_value()) {
      return std::unexpected(start_magic_result.error());
    }
    if (std::memcmp(start_magic_result.value().data(), kParquetMagic,
                    kMagicSize) != 0) {
      return Status::ParquetFormatError(
          "Invalid Parquet file: missing start magic");
    }

    // Check magic at end.
    auto end_magic_result = file_.Read(file_.size() - kMagicSize, kMagicSize);
    if (!end_magic_result.has_value()) {
      return std::unexpected(end_magic_result.error());
    }
    if (std::memcmp(end_magic_result.value().data(), kParquetMagic,
                    kMagicSize) != 0) {
      return Status::ParquetFormatError(
          "Invalid Parquet file: missing end magic");
    }

    return Status::OK();
  }

  [[nodiscard]] Status ReadMetadata() {
    // Read footer length (4 bytes before final magic).
    auto footer_len_result = file_.Read(file_.size() - kFooterSize, 4);
    if (!footer_len_result.has_value()) {
      return std::unexpected(footer_len_result.error());
    }
    uint32_t footer_length;
    std::memcpy(&footer_length, footer_len_result.value().data(), 4);

    // Read footer metadata.
    size_t footer_start = file_.size() - kFooterSize - footer_length;
    auto footer_data_result = file_.Read(footer_start, footer_length);
    if (!footer_data_result.has_value()) {
      return std::unexpected(footer_data_result.error());
    }

    // Parse Thrift-encoded metadata.
    auto metadata_result = MetadataParser::Parse(footer_data_result.value());
    if (!metadata_result.has_value()) {
      return std::unexpected(metadata_result.error());
    }
    metadata_ = std::move(metadata_result.value());

    // Build schema from metadata.
    schema_ = BuildSchema(metadata_);

    return Status::OK();
  }

  [[nodiscard]] Result<std::vector<uint8_t>> ReadColumnChunk(
      const ColumnChunkMetadata& column_meta) {
    // Determine start offset (dictionary page if present, else data page).
    int64_t start_offset = column_meta.dictionary_page_offset.value_or(
        column_meta.data_page_offset);
    int64_t length = column_meta.total_compressed_size;

    return file_.Read(static_cast<size_t>(start_offset),
                      static_cast<size_t>(length));
  }

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  FileHandle file_;
  FileMetadata metadata_;
  Schema schema_;
};

}  // namespace quest::datasource

#endif  // PARQUET_READER_FILE_READER_HPP
