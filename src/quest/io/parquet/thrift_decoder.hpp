// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

/// @file thrift_decoder.hpp
/// @brief Thrift Compact Protocol decoder for Parquet metadata.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "quest/common/result.hpp"

namespace quest::io::parquet {

// =============================================================================
// Thrift Protocol Types
// =============================================================================

enum class ThriftType : uint8_t {
  kStop = 0,       ///< End of struct marker.
  kBoolTrue = 1,   ///< Boolean true value.
  kBoolFalse = 2,  ///< Boolean false value.
  kByte = 3,       ///< 8-bit signed integer.
  kI16 = 4,        ///< 16-bit signed integer (varint encoded).
  kI32 = 5,        ///< 32-bit signed integer (varint encoded).
  kI64 = 6,        ///< 64-bit signed integer (varint encoded).
  kDouble = 7,     ///< 64-bit IEEE double.
  kBinary = 8,     ///< Binary blob or UTF-8 string.
  kList = 9,       ///< Ordered list of elements.
  kSet = 10,       ///< Unordered set of elements.
  kMap = 11,       ///< Key-value map.
  kStruct = 12     ///< Nested struct.
};

// =============================================================================
// ThriftDecoder Class
// =============================================================================

class ThriftDecoder {
 public:
  explicit ThriftDecoder(std::span<const uint8_t> data) noexcept
      : data_(data), pos_(0) {}

  // ===========================================================================
  // Position Queries
  // ===========================================================================

  [[nodiscard]] size_t position() const noexcept { return pos_; }

  [[nodiscard]] size_t remaining() const noexcept {
    return data_.size() - pos_;
  }

  // ===========================================================================
  // Basic Read Operations
  // ===========================================================================

  [[nodiscard]] Status Skip(size_t count) noexcept;

  [[nodiscard]] Result<uint8_t> ReadByte() noexcept;

  // ===========================================================================
  // Integer Read Operations
  // ===========================================================================

  /// @brief Reads a zigzag-encoded 32-bit varint.
  [[nodiscard]] Result<int32_t> ReadVarint32() noexcept;

  /// @brief Reads a zigzag-encoded 64-bit varint.
  [[nodiscard]] Result<int64_t> ReadVarint64() noexcept;

  /// @brief Reads an unsigned 32-bit varint (used for lengths).
  [[nodiscard]] Result<uint32_t> ReadUvarint32() noexcept;

  [[nodiscard]] Result<int16_t> ReadI16() noexcept;

  [[nodiscard]] Result<int32_t> ReadI32() noexcept { return ReadVarint32(); }

  [[nodiscard]] Result<int64_t> ReadI64() noexcept { return ReadVarint64(); }

  // ===========================================================================
  // String and Binary Read Operations
  // ===========================================================================

  /// @brief Reads a length-prefixed UTF-8 string.
  [[nodiscard]] Result<std::string> ReadString() noexcept;

  /// @brief Reads a length-prefixed binary blob.
  [[nodiscard]] Result<std::vector<uint8_t>> ReadBinary() noexcept;

  // ===========================================================================
  // Struct Field Header
  // ===========================================================================

  struct FieldHeader {
    int16_t field_id;
    ThriftType type;
  };

  /// @param last_field_id Previous field ID used for delta decoding.
  /// @return nullopt on STOP field.
  [[nodiscard]] Result<std::optional<FieldHeader>> ReadFieldHeader(
      int16_t last_field_id) noexcept;

  // ===========================================================================
  // List/Set Header
  // ===========================================================================

  struct ListHeader {
    ThriftType element_type;
    uint32_t size;
  };

  [[nodiscard]] Result<ListHeader> ReadListHeader() noexcept;

  // ===========================================================================
  // Field Skipping
  // ===========================================================================

  [[nodiscard]] Status SkipField(ThriftType type) noexcept;

  /// @brief Skips an entire struct, reading fields until STOP.
  [[nodiscard]] Status SkipStruct() noexcept;

 private:
  std::span<const uint8_t> data_;
  size_t pos_;
};

}  // namespace quest::io::parquet
