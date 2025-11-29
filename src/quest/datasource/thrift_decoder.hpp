// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_THRIFT_DECODER_HPP
#define PARQUET_READER_THRIFT_DECODER_HPP

/// @file thrift_decoder.hpp
/// @brief Thrift Compact Protocol decoder for Parquet metadata.
///
/// This file implements a decoder for the Thrift Compact Protocol used in
/// Parquet file metadata. It provides safe, error-handling reads using the
/// Result type pattern instead of exceptions.

#include <bit>
#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "quest/result.hpp"

namespace quest::datasource {

// =============================================================================
// Thrift Protocol Types
// =============================================================================

/// @brief Thrift Compact Protocol type IDs.
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

/// @brief Decoder for Thrift Compact Protocol used in Parquet metadata.
///
/// This decoder reads Thrift-encoded data from a byte buffer, supporting
/// all Compact Protocol types. All read operations return Result types
/// to safely handle decode errors.
///
/// Example usage:
/// @code
///   ThriftDecoder decoder(metadata_bytes);
///   auto version_result = decoder.ReadI32();
///   if (!version_result.has_value()) {
///     return std::unexpected(version_result.error());
///   }
///   int32_t version = version_result.value();
/// @endcode
class ThriftDecoder {
 public:
  /// @brief Constructs a decoder over a span of bytes.
  /// @param data The byte buffer to decode from.
  explicit ThriftDecoder(std::span<const uint8_t> data) noexcept
      : data_(data), pos_(0) {}

  // ===========================================================================
  // Position Queries
  // ===========================================================================

  /// @brief Returns the current read position in bytes.
  [[nodiscard]] size_t position() const noexcept { return pos_; }

  /// @brief Returns the number of bytes remaining to read.
  [[nodiscard]] size_t remaining() const noexcept {
    return data_.size() - pos_;
  }

  /// @brief Checks if there are more bytes to read.
  [[nodiscard]] bool HasMore() const noexcept { return pos_ < data_.size(); }

  // ===========================================================================
  // Basic Read Operations
  // ===========================================================================

  /// @brief Skips a specified number of bytes.
  /// @param count Number of bytes to skip.
  /// @return Status indicating success or end-of-data error.
  [[nodiscard]] Status Skip(size_t count) noexcept {
    if (pos_ + count > data_.size()) {
      return Status::ThriftDecodeError("Unexpected end of data during skip");
    }
    pos_ += count;
    return Status::OK();
  }

  /// @brief Reads a single byte.
  /// @return The byte value, or error if end of data.
  [[nodiscard]] Result<uint8_t> ReadByte() noexcept {
    if (pos_ >= data_.size()) {
      return std::unexpected(
          Status::ThriftDecodeError("Unexpected end of data reading byte"));
    }
    return data_[pos_++];
  }

  // ===========================================================================
  // Integer Read Operations
  // ===========================================================================

  /// @brief Reads a zigzag-encoded 32-bit varint.
  /// @return The decoded signed 32-bit integer, or decode error.
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
            Status::ThriftDecodeError("Varint32 too long (exceeds 5 bytes)"));
      }
    }

    // Zigzag decode: convert unsigned to signed.
    return static_cast<int32_t>((result >> 1) ^ -(result & 1));
  }

  /// @brief Reads a zigzag-encoded 64-bit varint.
  /// @return The decoded signed 64-bit integer, or decode error.
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
            Status::ThriftDecodeError("Varint64 too long (exceeds 10 bytes)"));
      }
    }

    // Zigzag decode: convert unsigned to signed.
    return static_cast<int64_t>((result >> 1) ^ -(result & 1));
  }

  /// @brief Reads an unsigned 32-bit varint (for lengths).
  /// @return The decoded unsigned 32-bit integer, or decode error.
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

  /// @brief Reads a 16-bit signed integer.
  [[nodiscard]] Result<int16_t> ReadI16() noexcept {
    auto result = ReadVarint32();
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    return static_cast<int16_t>(result.value());
  }

  /// @brief Reads a 32-bit signed integer.
  [[nodiscard]] Result<int32_t> ReadI32() noexcept { return ReadVarint32(); }

  /// @brief Reads a 64-bit signed integer.
  [[nodiscard]] Result<int64_t> ReadI64() noexcept { return ReadVarint64(); }

  // ===========================================================================
  // Floating-Point Read Operations
  // ===========================================================================

  /// @brief Reads a 64-bit IEEE double.
  /// @return The decoded double, or decode error.
  [[nodiscard]] Result<double> ReadDouble() noexcept {
    if (pos_ + 8 > data_.size()) {
      return std::unexpected(
          Status::ThriftDecodeError("Unexpected end of data reading double"));
    }

    uint64_t bits;
    std::memcpy(&bits, &data_[pos_], 8);
    pos_ += 8;

    return std::bit_cast<double>(bits);
  }

  // ===========================================================================
  // String and Binary Read Operations
  // ===========================================================================

  /// @brief Reads a length-prefixed UTF-8 string.
  /// @return The decoded string, or decode error.
  [[nodiscard]] Result<std::string> ReadString() noexcept {
    auto length_result = ReadUvarint32();
    if (!length_result.has_value()) {
      return std::unexpected(length_result.error());
    }
    uint32_t length = length_result.value();

    if (pos_ + length > data_.size()) {
      return Status::ThriftDecodeError(
          "String length exceeds remaining buffer");
    }

    std::string result(reinterpret_cast<const char*>(&data_[pos_]), length);
    pos_ += length;
    return result;
  }

  /// @brief Reads a length-prefixed binary blob.
  /// @return The decoded bytes, or decode error.
  [[nodiscard]] Result<std::vector<uint8_t>> ReadBinary() noexcept {
    auto length_result = ReadUvarint32();
    if (!length_result.has_value()) {
      return std::unexpected(length_result.error());
    }
    uint32_t length = length_result.value();

    if (pos_ + length > data_.size()) {
      return Status::ThriftDecodeError(
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

  /// @brief Field header containing field ID and type.
  struct FieldHeader {
    int16_t field_id;  ///< The field identifier.
    ThriftType type;   ///< The field's Thrift type.
  };

  /// @brief Reads a field header in Compact Protocol format.
  /// @param last_field_id The previous field ID (for delta encoding).
  /// @return The field header, or nullopt for STOP field, or decode error.
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

  /// @brief List or set header containing element type and count.
  struct ListHeader {
    ThriftType element_type;  ///< Type of list elements.
    uint32_t size;            ///< Number of elements.
  };

  /// @brief Reads a list or set header.
  /// @return The list header, or decode error.
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

  /// @brief Skips a field of the given Thrift type.
  /// @param type The type of field to skip.
  /// @return Status indicating success or decode error.
  [[nodiscard]] Status SkipField(ThriftType type) noexcept {
    switch (type) {
      case ThriftType::kBoolTrue:
      case ThriftType::kBoolFalse:
        // No data to skip for inline booleans.
        return Status::OK();

      case ThriftType::kByte: {
        auto status = Skip(1);
        if (!status.ok()) {
          return status;
        }
        return Status::OK();
      }

      case ThriftType::kI16:
      case ThriftType::kI32: {
        auto result = ReadVarint32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        return Status::OK();
      }

      case ThriftType::kI64: {
        auto result = ReadVarint64();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        return Status::OK();
      }

      case ThriftType::kDouble: {
        auto status = Skip(8);
        if (!status.ok()) {
          return status;
        }
        return Status::OK();
      }

      case ThriftType::kBinary: {
        auto length_result = ReadUvarint32();
        if (!length_result.has_value()) {
          return std::unexpected(length_result.error());
        }
        auto status = Skip(length_result.value());
        if (!status.ok()) {
          return status;
        }
        return Status::OK();
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
          if (!status.ok()) {
            return status;
          }
        }
        return Status::OK();
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
            if (!key_status.ok()) {
              return key_status;
            }
            auto val_status = SkipField(val_type);
            if (!val_status.ok()) {
              return val_status;
            }
          }
        }
        return Status::OK();
      }

      case ThriftType::kStruct:
        return SkipStruct();

      default:
        return Status::ThriftDecodeError("Unknown Thrift type to skip");
    }
  }

  /// @brief Skips an entire struct (reads fields until STOP).
  /// @return Status indicating success or decode error.
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
      if (!status.ok()) {
        return status;
      }
    }

    return Status::OK();
  }

 private:
  std::span<const uint8_t> data_;  ///< The byte buffer being decoded.
  size_t pos_;                     ///< Current read position.
};

}  // namespace quest::datasource

#endif  // PARQUET_READER_THRIFT_DECODER_HPP
