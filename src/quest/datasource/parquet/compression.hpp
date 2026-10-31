// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

/// @file compression.hpp
/// @brief Compression codec abstraction for Parquet decompression. Currently supports Snappy.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <snappy.h>

#include "quest/datasource/types.hpp"
#include "quest/result.hpp"

namespace quest::datasource::parquet {

class CompressionCodec {
 public:
  virtual ~CompressionCodec() = default;

  [[nodiscard]] virtual Result<std::vector<uint8_t>> Decompress(
      std::span<const uint8_t> compressed,
      size_t uncompressed_size) const = 0;

  [[nodiscard]] virtual CompressionType type() const = 0;
};

/// @brief Uncompressed codec - just copies the data.
class UncompressedCodec : public CompressionCodec {
 public:
  [[nodiscard]] Result<std::vector<uint8_t>> Decompress(
      std::span<const uint8_t> compressed,
      size_t uncompressed_size) const override {
    if (compressed.size() != uncompressed_size) {
      return std::unexpected(Status::Invalid(
          "Uncompressed size mismatch: expected " +
          std::to_string(uncompressed_size) + " but got " +
          std::to_string(compressed.size())));
    }
    return std::vector<uint8_t>(compressed.begin(), compressed.end());
  }

  [[nodiscard]] CompressionType type() const override {
    return CompressionType::kUncompressed;
  }
};

class SnappyCodec : public CompressionCodec {
 public:
  [[nodiscard]] Result<std::vector<uint8_t>> Decompress(
      std::span<const uint8_t> compressed,
      size_t uncompressed_size) const override {
    std::vector<uint8_t> output(uncompressed_size);

    bool success = snappy::RawUncompress(
        reinterpret_cast<const char*>(compressed.data()), compressed.size(),
        reinterpret_cast<char*>(output.data()));

    if (!success) {
      return std::unexpected(
          Status::Invalid("Failed to decompress Snappy data"));
    }

    return output;
  }

  [[nodiscard]] CompressionType type() const override {
    return CompressionType::kSnappy;
  }
};

/// @return Error for unsupported compression types.
[[nodiscard]] inline Result<std::unique_ptr<CompressionCodec>> CreateCodec(
    CompressionType type) {
  switch (type) {
    case CompressionType::kUncompressed:
      return std::make_unique<UncompressedCodec>();
    case CompressionType::kSnappy:
      return std::make_unique<SnappyCodec>();
    case CompressionType::kGzip:
      return std::unexpected(
          Status::UnsupportedEncoding("GZIP compression not yet supported"));
    case CompressionType::kLzo:
      return std::unexpected(
          Status::UnsupportedEncoding("LZO compression not yet supported"));
    case CompressionType::kBrotli:
      return std::unexpected(
          Status::UnsupportedEncoding("Brotli compression not yet supported"));
    case CompressionType::kLz4:
      return std::unexpected(
          Status::UnsupportedEncoding("LZ4 compression not yet supported"));
    case CompressionType::kZstd:
      return std::unexpected(
          Status::UnsupportedEncoding("ZSTD compression not yet supported"));
    case CompressionType::kLz4Raw:
      return std::unexpected(
          Status::UnsupportedEncoding("LZ4_RAW compression not yet supported"));
  }
  return std::unexpected(
      Status::UnsupportedEncoding("Unknown compression type"));
}

}  // namespace quest::datasource::parquet
