// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/io/parquet/file_reader.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <utility>

#include "quest/io/parquet/bytes.hpp"
#include "quest/io/parquet/column_reader.hpp"
#include "quest/io/parquet/compression.hpp"

namespace quest::io::parquet {

Result<FileHandle> FileHandle::Open(const std::string& path) {
  FileHandle handle;
  handle.path_ = path;
  handle.fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (handle.fd_ < 0) {
    return Error::IOError("Failed to open file: " + path);
  }
  struct stat info {};
  if (::fstat(handle.fd_, &info) != 0) {
    return Error::IOError("Failed to stat file: " + path);
  }
  handle.file_size_ = static_cast<size_t>(info.st_size);
  return handle;
}

FileHandle::FileHandle(FileHandle&& other) noexcept
    : path_(std::move(other.path_)),
      fd_(std::exchange(other.fd_, -1)),
      file_size_(other.file_size_) {}

FileHandle& FileHandle::operator=(FileHandle&& other) noexcept {
  if (this != &other) {
    if (fd_ >= 0) {
      ::close(fd_);
    }
    path_ = std::move(other.path_);
    fd_ = std::exchange(other.fd_, -1);
    file_size_ = other.file_size_;
  }
  return *this;
}

FileHandle::~FileHandle() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

Result<std::vector<uint8_t>> FileHandle::Read(size_t offset, size_t length) const {
  if (offset > file_size_ || length > file_size_ - offset) {
    return Error::OutOfRange("Read past end of file");
  }
  std::vector<uint8_t> buffer(length);
  size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd_, buffer.data() + done, length - done,
                              static_cast<off_t>(offset + done));
    if (n < 0 && errno == EINTR) {
      continue;
    }
    if (n <= 0) {
      return Error::IOError("Failed to read from file: " + path_);
    }
    done += static_cast<size_t>(n);
  }
  return buffer;
}

Result<ParquetFileReader> ParquetFileReader::Open(const std::string& path) {
  ParquetFileReader reader;

  auto file_result = FileHandle::Open(path);
  if (!file_result.has_value()) {
    return std::unexpected(file_result.error());
  }
  reader.file_ = std::move(file_result.value());

  auto validate_status = reader.ValidateFileFormat();
  if (!validate_status.has_value()) {
    return std::unexpected(validate_status.error());
  }

  auto metadata_status = reader.ReadMetadata();
  if (!metadata_status.has_value()) {
    return std::unexpected(metadata_status.error());
  }

  return reader;
}

Result<RecordBatch> ParquetFileReader::ReadRowGroup(
    size_t row_group_index,
    const std::vector<std::string>& columns) {
  if (row_group_index >= metadata_.row_groups.size()) {
    return std::unexpected(
        Error::OutOfRange("Row group index out of range"));
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
            Error::NotFound("Column not found: " + name));
      }
      column_indices.push_back(*idx);
    }
    auto selected = schema_.Select(columns);
    if (!selected) {
      return std::unexpected(selected.error());
    }
    projected_schema = std::move(*selected);
  }

  std::vector<ColumnVectorPtr> column_vectors;
  column_vectors.reserve(column_indices.size());

  for (size_t col_idx : column_indices) {
    if (col_idx >= row_group.columns.size()) {
      return Error::ParquetFormatError("Row group has fewer column chunks than schema columns");
    }
    const auto& column_meta = row_group.columns[col_idx];

    auto column_data_result = ReadColumnChunk(column_meta);
    if (!column_data_result.has_value()) {
      return std::unexpected(column_data_result.error());
    }

    auto codec_result = CreateCodec(column_meta.codec);
    if (!codec_result.has_value()) {
      return std::unexpected(codec_result.error());
    }

    ColumnReader reader(schema_.field(col_idx), leaves_[col_idx], column_meta,
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

Status ParquetFileReader::ValidateFileFormat() {
  if (file_.size() < kMagicSize * 2 + 4) {
    return Error::ParquetFormatError(
        "File too small to be valid Parquet");
  }

  auto start_magic_result = file_.Read(0, kMagicSize);
  if (!start_magic_result.has_value()) {
    return std::unexpected(start_magic_result.error());
  }
  if (std::memcmp(start_magic_result.value().data(), kParquetMagic,
                  kMagicSize) != 0) {
    return Error::ParquetFormatError(
        "Invalid Parquet file: missing start magic");
  }

  auto end_magic_result = file_.Read(file_.size() - kMagicSize, kMagicSize);
  if (!end_magic_result.has_value()) {
    return std::unexpected(end_magic_result.error());
  }
  if (std::memcmp(end_magic_result.value().data(), kParquetMagic,
                  kMagicSize) != 0) {
    return Error::ParquetFormatError(
        "Invalid Parquet file: missing end magic");
  }

  return {};
}

Status ParquetFileReader::ReadMetadata() {
  auto footer_len_result = file_.Read(file_.size() - kFooterSize, 4);
  if (!footer_len_result.has_value()) {
    return std::unexpected(footer_len_result.error());
  }
  auto footer_length_result = LoadLE<uint32_t>(footer_len_result.value(), 0);
  if (!footer_length_result) {
    return std::unexpected(std::move(footer_length_result).error());
  }
  uint32_t footer_length = *footer_length_result;
  if (footer_length > file_.size() - kFooterSize - kMagicSize) {
    return Error::ParquetFormatError("Invalid Parquet file: footer length exceeds file size");
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

  auto file_schema = BuildSchema(metadata_);
  if (!file_schema) {
    return std::unexpected(file_schema.error());
  }
  schema_ = std::move(file_schema->schema);
  leaves_ = std::move(file_schema->leaves);

  return {};
}

Result<std::vector<uint8_t>> ParquetFileReader::ReadColumnChunk(
    const ColumnChunkMetadata& column_meta) {
  // Determine start offset (dictionary page if present, else data page).
  int64_t start_offset = column_meta.dictionary_page_offset.value_or(
      column_meta.data_page_offset);
  int64_t length = column_meta.total_compressed_size;

  return file_.Read(static_cast<size_t>(start_offset),
                    static_cast<size_t>(length));
}

}  // namespace quest::io::parquet
