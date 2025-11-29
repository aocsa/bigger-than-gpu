// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_COMPRESSION_HPP
#define PARQUET_READER_COMPRESSION_HPP

/// @file compression.hpp
/// @brief Compression codec abstraction for Parquet decompression.
///
/// This file provides a unified interface for decompressing data pages
/// in Parquet files. Currently supports Snappy compression.

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include <snappy.h>

#include "quest/datasource/types.hpp"
#include "quest/result.hpp"

namespace quest::datasource {

/// @brief Abstract base class for compression codecs.
class CompressionCodec {
 public:
  virtual ~CompressionCodec() = default;

  /// @brief Decompress data from source to destination.
  /// @param compressed The compressed data.
  /// @param uncompressed_size The expected uncompressed size.
  /// @return Decompressed data or error status.
  [[nodiscard]] virtual Result<std::vector<uint8_t>> Decompress(
      std::span<const uint8_t> compressed,
      size_t uncompressed_size) const = 0;

  /// @brief Get the compression type.
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

/// @brief Snappy compression codec.
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

/// @brief Factory function to create a compression codec.
/// @param type The compression type.
/// @return A codec instance or error for unsupported types.
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

/// @brief Get human-readable name for compression type.
[[nodiscard]] inline std::string CompressionTypeName(CompressionType type) {
  switch (type) {
    case CompressionType::kUncompressed:
      return "UNCOMPRESSED";
    case CompressionType::kSnappy:
      return "SNAPPY";
    case CompressionType::kGzip:
      return "GZIP";
    case CompressionType::kLzo:
      return "LZO";
    case CompressionType::kBrotli:
      return "BROTLI";
    case CompressionType::kLz4:
      return "LZ4";
    case CompressionType::kZstd:
      return "ZSTD";
    case CompressionType::kLz4Raw:
      return "LZ4_RAW";
  }
  return "UNKNOWN";
}

}  // namespace quest::datasource

#endif  // PARQUET_READER_COMPRESSION_HPP
