// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_ENCODING_HPP
#define PARQUET_READER_ENCODING_HPP

/// @file encoding.hpp
/// @brief Value decoders for Parquet encodings (Plain, RLE, Dictionary).

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/parquet/bytes.hpp"
#include "quest/datasource/types.hpp"
#include "quest/result.hpp"

namespace quest::datasource::parquet {

// =============================================================================
// Base Value Decoder
// =============================================================================

class ValueDecoder {
 public:
  virtual ~ValueDecoder() = default;

  virtual void SetData(std::span<const uint8_t> data) = 0;

  [[nodiscard]] virtual size_t ValuesRemaining() const = 0;
};

// =============================================================================
// Plain Encoding Decoder
// =============================================================================

/// @brief Plain encoding decoder for fixed-width types (raw binary, no compression).
/// @tparam T The value type (int32_t, int64_t, float, double, etc.)
template <typename T>
class PlainDecoder : public ValueDecoder {
 public:
  void SetData(std::span<const uint8_t> data) override {
    data_ = data;
    pos_ = 0;
    num_values_ = data.size() / sizeof(T);
    values_read_ = 0;
  }

  [[nodiscard]] size_t ValuesRemaining() const override {
    return num_values_ - values_read_;
  }

  [[nodiscard]] Result<T> ReadValue() {
    if (values_read_ >= num_values_) {
      return std::unexpected(
          Status::ColumnDecodeError("No more values to read"));
    }
    auto value = LoadLE<T>(data_, pos_);
    if (!value) {
      return value;
    }
    pos_ += sizeof(T);
    ++values_read_;
    return value;
  }

  size_t ReadBatch(std::span<T> values) {
    size_t count = std::min(values.size(), ValuesRemaining());
    std::memcpy(values.data(), data_.data() + pos_, count * sizeof(T));
    pos_ += count * sizeof(T);
    values_read_ += count;
    return count;
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  size_t num_values_ = 0;
  size_t values_read_ = 0;
};

// =============================================================================
// Plain Boolean Decoder (Bit-Packed)
// =============================================================================

/// @brief Plain decoder specialization for boolean type (bit-packed).
///
/// Boolean values in Parquet are packed 8 per byte in plain encoding.
template <>
class PlainDecoder<bool> : public ValueDecoder {
 public:
  void SetData(std::span<const uint8_t> data) override {
    data_ = data;
    byte_pos_ = 0;
    bit_pos_ = 0;
    values_read_ = 0;
  }

  void SetNumValues(size_t num_values) { num_values_ = num_values; }

  [[nodiscard]] size_t ValuesRemaining() const override {
    return num_values_ - values_read_;
  }

  [[nodiscard]] Result<bool> ReadValue() {
    if (values_read_ >= num_values_) {
      return std::unexpected(
          Status::ColumnDecodeError("No more boolean values to read"));
    }
    if (byte_pos_ >= data_.size()) {
      return Status::ColumnDecodeError("Not enough data for boolean values");
    }
    bool value = (data_[byte_pos_] & (1 << bit_pos_)) != 0;
    ++bit_pos_;
    if (bit_pos_ == 8) {
      bit_pos_ = 0;
      ++byte_pos_;
    }
    ++values_read_;
    return value;
  }

 private:
  std::span<const uint8_t> data_;
  size_t byte_pos_ = 0;
  size_t bit_pos_ = 0;
  size_t num_values_ = 0;
  size_t values_read_ = 0;
};

// =============================================================================
// Plain Binary/String Decoder
// =============================================================================

/// @brief Plain decoder for variable-length binary/string types.
///
/// Binary values are stored as a 4-byte little-endian length followed
/// by the raw bytes.
class PlainBinaryDecoder : public ValueDecoder {
 public:
  void SetData(std::span<const uint8_t> data) override {
    data_ = data;
    pos_ = 0;
  }

  [[nodiscard]] size_t ValuesRemaining() const override {
    return pos_ < data_.size() ? 1 : 0;  // Approximate.
  }

  [[nodiscard]] Result<std::string> ReadValue() {
    auto length = LoadLE<uint32_t>(data_, pos_);
    if (!length) {
      return std::unexpected(std::move(length).error());
    }
    auto bytes = Slice(data_, pos_ + 4, *length);
    if (!bytes) {
      return std::unexpected(std::move(bytes).error());
    }
    pos_ += 4 + *length;
    return std::string(reinterpret_cast<const char*>(bytes->data()), bytes->size());
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
};

// =============================================================================
// RLE/Bit-Packing Hybrid Decoder
// =============================================================================

/// @brief RLE/Bit-packing hybrid decoder for definition/repetition levels.
///
/// This decoder implements the hybrid RLE/bit-packing encoding used in
/// Parquet for definition and repetition levels. Values are either:
/// - Run-length encoded: repeated value stored once with count
/// - Bit-packed: 8 values packed together using minimal bits
class RleBitPackingDecoder {
 public:
  explicit RleBitPackingDecoder(int bit_width) : bit_width_(bit_width) {}

  void SetData(std::span<const uint8_t> data, size_t num_values) {
    data_ = data;
    pos_ = 0;
    num_values_ = num_values;
    values_read_ = 0;
    current_count_ = 0;
    current_value_ = 0;
    is_rle_ = false;
    bit_packed_pos_ = 0;
    bit_packed_count_ = 0;
  }

  [[nodiscard]] size_t ValuesRemaining() const {
    return num_values_ - values_read_;
  }

  [[nodiscard]] Result<int32_t> ReadValue() {
    if (current_count_ == 0) {
      auto status = ReadNextGroup();
      if (!status.ok()) {
        return std::unexpected(status.error());
      }
    }

    int32_t value = current_value_;
    if (!is_rle_) {
      auto packed = ReadBitPackedValue();
      if (!packed) {
        return packed;
      }
      value = *packed;
    }
    --current_count_;
    ++values_read_;
    return value;
  }

  [[nodiscard]] Status ReadBatch(std::span<int32_t> values) {
    for (size_t i = 0; i < values.size(); ++i) {
      auto result = ReadValue();
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      values[i] = result.value();
    }
    return Status::OK();
  }

 private:
  [[nodiscard]] Status ReadNextGroup() {
    if (pos_ >= data_.size()) {
      return Status::ColumnDecodeError("RLE decoder: unexpected end of data");
    }
    if (bit_width_ < 0 || bit_width_ > 32) {
      return Status::ColumnDecodeError(std::format("RLE decoder: invalid bit width {}", bit_width_));
    }

    auto varint = ReadVarInt();
    if (!varint) {
      return std::unexpected(std::move(varint).error());
    }
    uint32_t indicator = *varint;
    if (indicator & 1) {
      // Bit-packed run.
      is_rle_ = false;
      bit_packed_count_ = ((indicator >> 1) * 8);
      current_count_ = bit_packed_count_;
      bit_packed_pos_ = 0;
      // Writers may truncate the final run's padding, so take what is there; reading past it is
      // caught per value.
      size_t bytes_needed = std::min((bit_packed_count_ * bit_width_ + 7) / 8, data_.size() - pos_);
      bit_packed_data_ = data_.subspan(pos_, bytes_needed);
      pos_ += bytes_needed;
    } else {
      // RLE run.
      is_rle_ = true;
      current_count_ = indicator >> 1;
      current_value_ = ReadFixedWidth();
    }
    return Status::OK();
  }

  [[nodiscard]] Result<uint32_t> ReadVarInt() {
    uint32_t result = 0;
    for (uint32_t shift = 0; shift < 32 && pos_ < data_.size(); shift += 7) {
      uint8_t byte = data_[pos_++];
      result |= static_cast<uint32_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        return result;
      }
    }
    return Status::ColumnDecodeError("RLE decoder: truncated or oversized varint");
  }

  int32_t ReadFixedWidth() {
    int32_t value = 0;
    size_t bytes_to_read = (bit_width_ + 7) / 8;
    for (size_t i = 0; i < bytes_to_read && pos_ < data_.size(); ++i) {
      value |= static_cast<int32_t>(data_[pos_++]) << (i * 8);
    }
    return value;
  }

  [[nodiscard]] Result<int32_t> ReadBitPackedValue() {
    size_t bit_offset = bit_packed_pos_ * bit_width_;
    size_t byte_offset = bit_offset / 8;
    size_t bit_in_byte = bit_offset % 8;
    if ((bit_offset + bit_width_ + 7) / 8 > bit_packed_data_.size()) {
      return Status::ColumnDecodeError("RLE decoder: bit-packed run exceeds data");
    }

    int32_t value = 0;
    int bits_read = 0;
    while (bits_read < bit_width_) {
      int bits_from_byte =
          std::min(8 - static_cast<int>(bit_in_byte), bit_width_ - bits_read);
      uint8_t mask = (1 << bits_from_byte) - 1;
      value |= ((bit_packed_data_[byte_offset] >> bit_in_byte) & mask)
               << bits_read;
      bits_read += bits_from_byte;
      bit_in_byte = 0;
      ++byte_offset;
    }
    ++bit_packed_pos_;
    return value;
  }

  int bit_width_;
  std::span<const uint8_t> data_;
  size_t pos_ = 0;
  size_t num_values_ = 0;
  size_t values_read_ = 0;

  size_t current_count_ = 0;
  int32_t current_value_ = 0;
  bool is_rle_ = false;

  std::span<const uint8_t> bit_packed_data_;
  size_t bit_packed_pos_ = 0;
  size_t bit_packed_count_ = 0;
};

// =============================================================================
// Utility Functions
// =============================================================================

[[nodiscard]] inline int ComputeBitWidth(int max_value) {
  if (max_value <= 0) {
    return 0;
  }
  return std::bit_width(static_cast<unsigned>(max_value));
}

}  // namespace quest::datasource::parquet

#endif  // PARQUET_READER_ENCODING_HPP
