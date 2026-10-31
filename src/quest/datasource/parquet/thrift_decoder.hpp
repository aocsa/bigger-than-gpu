// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

/// @file thrift_decoder.hpp
/// @brief Thrift Compact Protocol decoder for Parquet metadata.

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "quest/result.hpp"

namespace quest::datasource::parquet {

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

  [[nodiscard]] Status Skip(size_t count) noexcept {
    if (pos_ + count > data_.size()) {
      return Error::ThriftDecodeError("Unexpected end of data during skip");
    }
    pos_ += count;
    return {};
  }

  [[nodiscard]] Result<uint8_t> ReadByte() noexcept {
    if (pos_ >= data_.size()) {
      return std::unexpected(
          Error::ThriftDecodeError("Unexpected end of data reading byte"));
    }
    return data_[pos_++];
  }

  // ===========================================================================
  // Integer Read Operations
  // ===========================================================================

  /// @brief Reads a zigzag-encoded 32-bit varint.
  [[nodiscard]] Result<int32_t> ReadVarint32() noexcept {
    uint32_t result = 0;
    uint32_t shift = 0;

    while (true) {
      auto byte_result = ReadByte();
      if (!byte_result.has_value()) {
        return std::unexpected(byte_result.error());
      }
      uint8_t byte = byte_result.value();

      result |= static_cast<uint32_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        break;
      }

      shift += 7;
      if (shift > 28) {
        return std::unexpected(
            Error::ThriftDecodeError("Varint32 too long (exceeds 5 bytes)"));
      }
    }

    // Zigzag decode: convert unsigned to signed.
    return static_cast<int32_t>((result >> 1) ^ -(result & 1));
  }

  /// @brief Reads a zigzag-encoded 64-bit varint.
  [[nodiscard]] Result<int64_t> ReadVarint64() noexcept {
    uint64_t result = 0;
    uint64_t shift = 0;

    while (true) {
      auto byte_result = ReadByte();
      if (!byte_result.has_value()) {
        return std::unexpected(byte_result.error());
      }
      uint8_t byte = byte_result.value();

      result |= static_cast<uint64_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        break;
      }

      shift += 7;
      if (shift > 63) {
        return std::unexpected(
            Error::ThriftDecodeError("Varint64 too long (exceeds 10 bytes)"));
      }
    }

    // Zigzag decode: convert unsigned to signed.
    return static_cast<int64_t>((result >> 1) ^ -(result & 1));
  }

  /// @brief Reads an unsigned 32-bit varint (used for lengths).
  [[nodiscard]] Result<uint32_t> ReadUvarint32() noexcept {
    uint32_t result = 0;
    uint32_t shift = 0;

    while (true) {
      auto byte_result = ReadByte();
      if (!byte_result.has_value()) {
        return std::unexpected(byte_result.error());
      }
      uint8_t byte = byte_result.value();

      result |= static_cast<uint32_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        break;
      }

      shift += 7;
    }

    return result;
  }

  [[nodiscard]] Result<int16_t> ReadI16() noexcept {
    auto result = ReadVarint32();
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    return static_cast<int16_t>(result.value());
  }

  [[nodiscard]] Result<int32_t> ReadI32() noexcept { return ReadVarint32(); }

  [[nodiscard]] Result<int64_t> ReadI64() noexcept { return ReadVarint64(); }

  // ===========================================================================
  // String and Binary Read Operations
  // ===========================================================================

  /// @brief Reads a length-prefixed UTF-8 string.
  [[nodiscard]] Result<std::string> ReadString() noexcept {
    auto length_result = ReadUvarint32();
    if (!length_result.has_value()) {
      return std::unexpected(length_result.error());
    }
    uint32_t length = length_result.value();

    if (pos_ + length > data_.size()) {
      return Error::ThriftDecodeError(
          "String length exceeds remaining buffer");
    }

    std::string result(reinterpret_cast<const char*>(&data_[pos_]), length);
    pos_ += length;
    return result;
  }

  /// @brief Reads a length-prefixed binary blob.
  [[nodiscard]] Result<std::vector<uint8_t>> ReadBinary() noexcept {
    auto length_result = ReadUvarint32();
    if (!length_result.has_value()) {
      return std::unexpected(length_result.error());
    }
    uint32_t length = length_result.value();

    if (pos_ + length > data_.size()) {
      return Error::ThriftDecodeError(
          "Binary length exceeds remaining buffer");
    }

    std::vector<uint8_t> result(data_.begin() + pos_,
                                data_.begin() + pos_ + length);
    pos_ += length;
    return result;
  }

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
      int16_t last_field_id) noexcept {
    auto byte_result = ReadByte();
    if (!byte_result.has_value()) {
      return std::unexpected(byte_result.error());
    }
    uint8_t byte = byte_result.value();

    // Check for STOP field (end of struct).
    if (byte == 0) {
      return std::nullopt;
    }

    // Extract type from low nibble.
    ThriftType type = static_cast<ThriftType>(byte & 0x0F);
    int16_t field_id;

    // Extract field ID delta from high nibble.
    int16_t delta = (byte >> 4) & 0x0F;
    if (delta == 0) {
      // Full field ID follows.
      auto id_result = ReadI16();
      if (!id_result.has_value()) {
        return std::unexpected(id_result.error());
      }
      field_id = id_result.value();
    } else {
      // Delta-encoded field ID.
      field_id = last_field_id + delta;
    }

    return FieldHeader{field_id, type};
  }

  // ===========================================================================
  // List/Set Header
  // ===========================================================================

  struct ListHeader {
    ThriftType element_type;
    uint32_t size;
  };

  [[nodiscard]] Result<ListHeader> ReadListHeader() noexcept {
    auto byte_result = ReadByte();
    if (!byte_result.has_value()) {
      return std::unexpected(byte_result.error());
    }
    uint8_t byte = byte_result.value();

    // Extract element type from low nibble.
    ThriftType elem_type = static_cast<ThriftType>(byte & 0x0F);

    // Extract size from high nibble (or read full varint if 0xF).
    uint32_t size = (byte >> 4) & 0x0F;
    if (size == 0x0F) {
      auto size_result = ReadUvarint32();
      if (!size_result.has_value()) {
        return std::unexpected(size_result.error());
      }
      size = size_result.value();
    }

    return ListHeader{elem_type, size};
  }

  // ===========================================================================
  // Field Skipping
  // ===========================================================================

  [[nodiscard]] Status SkipField(ThriftType type) noexcept {
    switch (type) {
      case ThriftType::kBoolTrue:
      case ThriftType::kBoolFalse:
        // No data to skip for inline booleans.
        return {};

      case ThriftType::kByte: {
        auto status = Skip(1);
        if (!status.has_value()) {
          return status;
        }
        return {};
      }

      case ThriftType::kI16:
      case ThriftType::kI32: {
        auto result = ReadVarint32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        return {};
      }

      case ThriftType::kI64: {
        auto result = ReadVarint64();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        return {};
      }

      case ThriftType::kDouble: {
        auto status = Skip(8);
        if (!status.has_value()) {
          return status;
        }
        return {};
      }

      case ThriftType::kBinary: {
        auto length_result = ReadUvarint32();
        if (!length_result.has_value()) {
          return std::unexpected(length_result.error());
        }
        auto status = Skip(length_result.value());
        if (!status.has_value()) {
          return status;
        }
        return {};
      }

      case ThriftType::kList:
      case ThriftType::kSet: {
        auto header_result = ReadListHeader();
        if (!header_result.has_value()) {
          return std::unexpected(header_result.error());
        }
        auto header = header_result.value();

        // Skip each element.
        for (uint32_t i = 0; i < header.size; ++i) {
          auto status = SkipField(header.element_type);
          if (!status.has_value()) {
            return status;
          }
        }
        return {};
      }

      case ThriftType::kMap: {
        auto size_result = ReadUvarint32();
        if (!size_result.has_value()) {
          return std::unexpected(size_result.error());
        }
        uint32_t size = size_result.value();

        if (size > 0) {
          auto types_result = ReadByte();
          if (!types_result.has_value()) {
            return std::unexpected(types_result.error());
          }
          uint8_t types = types_result.value();

          ThriftType key_type = static_cast<ThriftType>((types >> 4) & 0x0F);
          ThriftType val_type = static_cast<ThriftType>(types & 0x0F);

          // Skip each key-value pair.
          for (uint32_t i = 0; i < size; ++i) {
            auto key_status = SkipField(key_type);
            if (!key_status.has_value()) {
              return key_status;
            }
            auto val_status = SkipField(val_type);
            if (!val_status.has_value()) {
              return val_status;
            }
          }
        }
        return {};
      }

      case ThriftType::kStruct:
        return SkipStruct();

      default:
        return Error::ThriftDecodeError("Unknown Thrift type to skip");
    }
  }

  /// @brief Skips an entire struct, reading fields until STOP.
  [[nodiscard]] Status SkipStruct() noexcept {
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }

      auto header = header_result.value();
      if (!header) {
        // STOP field encountered.
        break;
      }

      last_field_id = header->field_id;
      auto status = SkipField(header->type);
      if (!status.has_value()) {
        return status;
      }
    }

    return {};
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_;
};

}  // namespace quest::datasource::parquet
