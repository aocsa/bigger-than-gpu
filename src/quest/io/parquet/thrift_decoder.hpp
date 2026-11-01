// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

/// @file thrift_decoder.hpp
/// @brief Thrift Compact Protocol decoder for Parquet metadata.

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
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
  /// @brief Skips one list, set or map element of type @p type.
  /// @note Unlike a bool field, whose value is its header's type, a bool element is one byte.
  [[nodiscard]] Status SkipElement(ThriftType type) noexcept;

  /// @brief Reads a length-prefixed run of bytes.
  /// @param too_long The error message when the run passes the end of the data.
  /// @return A view of the bytes in the decoder's buffer, or `ThriftDecodeError`.
  [[nodiscard]] Result<std::span<const uint8_t>> ReadLengthPrefixed(const char* too_long) noexcept;

  std::span<const uint8_t> data_;
  size_t pos_;
};

// =============================================================================
// Struct and Value Readers
// =============================================================================

/// @brief Reads one struct up to its STOP field.
/// @param on_field Called as `on_field(int16_t field_id, ThriftType type) -> Status` for each
/// field; it reads or skips the field's value.
/// @return The first error of the decoder or of @p on_field.
template <typename OnField>
[[nodiscard]] Status ReadStruct(ThriftDecoder& decoder, OnField&& on_field) {
  int16_t last_field_id = 0;
  while (true) {
    auto header = decoder.ReadFieldHeader(last_field_id);
    if (!header) {
      return std::unexpected(std::move(header).error());
    }
    if (!*header) {
      return {};
    }
    last_field_id = (*header)->field_id;
    if (auto read = on_field(last_field_id, (*header)->type); !read) {
      return read;
    }
  }
}

// =============================================================================
// Read(decoder, out): one overload per field type. Each reads one value from @p decoder
// into @p out and returns the decoder's error, if any. A struct type adds its own overload.
// =============================================================================

/// @brief Reads an i16 into @p out.
[[nodiscard]] inline Status Read(ThriftDecoder& decoder, int16_t& out) {
  return decoder.ReadI16().transform([&](int16_t value) { out = value; });
}

/// @brief Reads an i32 into @p out.
[[nodiscard]] inline Status Read(ThriftDecoder& decoder, int32_t& out) {
  return decoder.ReadI32().transform([&](int32_t value) { out = value; });
}

/// @brief Reads an i64 into @p out.
[[nodiscard]] inline Status Read(ThriftDecoder& decoder, int64_t& out) {
  return decoder.ReadI64().transform([&](int64_t value) { out = value; });
}

/// @brief Reads a thrift enum, an i32, into @p out.
template <typename E>
  requires std::is_enum_v<E>
[[nodiscard]] Status Read(ThriftDecoder& decoder, E& out) {
  return decoder.ReadI32().transform([&](int32_t value) { out = static_cast<E>(value); });
}

/// @brief Reads a string into @p out.
[[nodiscard]] inline Status Read(ThriftDecoder& decoder, std::string& out) {
  return decoder.ReadString().transform([&](std::string value) { out = std::move(value); });
}

/// @brief Reads a binary blob into @p out.
[[nodiscard]] inline Status Read(ThriftDecoder& decoder, std::vector<uint8_t>& out) {
  return decoder.ReadBinary().transform(
      [&](std::vector<uint8_t> value) { out = std::move(value); });
}

/// @brief Reads a value of an optional field into @p out, replacing any earlier value.
template <typename T>
[[nodiscard]] Status Read(ThriftDecoder& decoder, std::optional<T>& out) {
  return Read(decoder, out.emplace());
}

/// @brief Reads a list into @p out, replacing its contents; each element by its `Read`.
template <typename T>
[[nodiscard]] Status Read(ThriftDecoder& decoder, std::vector<T>& out) {
  auto header = decoder.ReadListHeader();
  if (!header) {
    return std::unexpected(std::move(header).error());
  }
  out.clear();
  out.reserve(header->size);
  for (uint32_t i = 0; i < header->size; ++i) {
    if (auto read = Read(decoder, out.emplace_back()); !read) {
      return read;
    }
  }
  return {};
}

}  // namespace quest::io::parquet
