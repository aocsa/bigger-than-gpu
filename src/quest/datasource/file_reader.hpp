// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "quest/datasource/column_reader.hpp"
#include "quest/datasource/parquet/compression.hpp"
#include "quest/datasource/parquet/metadata.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/result.hpp"

namespace quest::datasource {

using parquet::CompressionCodec;
using parquet::CreateCodec;
using parquet::FileMetadata;
using parquet::RowGroupMetadata;
using parquet::ColumnChunkMetadata;
using parquet::MetadataParser;
using parquet::BuildSchema;

// =============================================================================
// Constants
// =============================================================================

constexpr char kParquetMagic[] = "PAR1";
constexpr size_t kMagicSize = 4;
constexpr size_t kFooterSize = 8;  // footer length (4) + magic (4)

// =============================================================================
// File Handle
// =============================================================================

class FileHandle {
 public:
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

  [[nodiscard]] size_t size() const noexcept { return file_size_; }

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
  [[nodiscard]] static Result<ParquetFileReader> Open(const std::string& path) {
    ParquetFileReader reader;

    auto file_result = FileHandle::Open(path);
    if (!file_result.has_value()) {
      return std::unexpected(file_result.error());
    }
    reader.file_ = std::move(file_result.value());

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
      const std::vector<std::string>& columns = {}) {
    if (row_group_index >= metadata_.row_groups.size()) {
      return std::unexpected(
          Status::OutOfRange("Row group index out of range"));
    }

    const auto& row_group = metadata_.row_groups[row_group_index];

    std::vector<size_t> column_indices;
    Schema projected_schema;

    if (columns.empty()) {
      for (size_t i = 0; i < schema_.num_fields(); ++i) {
        column_indices.push_back(i);
      }
      projected_schema = schema_;
    } else {
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

    std::vector<std::shared_ptr<ColumnVector>> column_vectors;
    column_vectors.reserve(column_indices.size());

    for (size_t col_idx : column_indices) {
      const auto& column_meta = row_group.columns[col_idx];

      auto column_data_result = ReadColumnChunk(column_meta);
      if (!column_data_result.has_value()) {
        return std::unexpected(column_data_result.error());
      }

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

 private:
  // ===========================================================================
  // Private Methods
  // ===========================================================================

  [[nodiscard]] Status ValidateFileFormat() {
    if (file_.size() < kMagicSize * 2 + 4) {
      return Status::ParquetFormatError(
          "File too small to be valid Parquet");
    }

    auto start_magic_result = file_.Read(0, kMagicSize);
    if (!start_magic_result.has_value()) {
      return std::unexpected(start_magic_result.error());
    }
    if (std::memcmp(start_magic_result.value().data(), kParquetMagic,
                    kMagicSize) != 0) {
      return Status::ParquetFormatError(
          "Invalid Parquet file: missing start magic");
    }

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
    auto footer_len_result = file_.Read(file_.size() - kFooterSize, 4);
    if (!footer_len_result.has_value()) {
      return std::unexpected(footer_len_result.error());
    }
    auto footer_length_result = parquet::LoadLE<uint32_t>(footer_len_result.value(), 0);
    if (!footer_length_result) {
      return std::unexpected(std::move(footer_length_result).error());
    }
    uint32_t footer_length = *footer_length_result;
    if (footer_length > file_.size() - kFooterSize - kMagicSize) {
      return Status::ParquetFormatError("Invalid Parquet file: footer length exceeds file size");
    }

    size_t footer_start = file_.size() - kFooterSize - footer_length;
    auto footer_data_result = file_.Read(footer_start, footer_length);
    if (!footer_data_result.has_value()) {
      return std::unexpected(footer_data_result.error());
    }

    auto metadata_result = MetadataParser::Parse(footer_data_result.value());
    if (!metadata_result.has_value()) {
      return std::unexpected(metadata_result.error());
    }
    metadata_ = std::move(metadata_result.value());

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

  FileHandle file_;
  FileMetadata metadata_;
  Schema schema_;
};

}  // namespace quest::datasource
