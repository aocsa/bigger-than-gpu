// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_ENCODING_HPP
#define PARQUET_READER_ENCODING_HPP

/// @file encoding.hpp
/// @brief Value decoders for Parquet encodings (Plain, RLE, Dictionary).
///
/// This file provides decoders for the various encoding schemes used in
/// Parquet files. All decoders return Result types for safe error handling.

#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/types.hpp"
#include "quest/result.hpp"

namespace quest::datasource::parquet {

// =============================================================================
// Base Value Decoder
// =============================================================================

/// @brief Base class for value decoders.
class ValueDecoder {
 public:
  virtual ~ValueDecoder() = default;

  /// @brief Sets the input data buffer.
  virtual void SetData(std::span<const uint8_t> data) = 0;

  /// @brief Returns the number of values remaining to read.
  [[nodiscard]] virtual size_t ValuesRemaining() const = 0;
};

// =============================================================================
// Plain Encoding Decoder
// =============================================================================

/// @brief Plain encoding decoder for fixed-width types.
///
/// This decoder reads values stored in their raw binary representation
/// without any compression or encoding.
///
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

  /// @brief Reads a single value.
  /// @return The value, or decode error if no more values.
  [[nodiscard]] Result<T> ReadValue() {
    if (values_read_ >= num_values_) {
      return std::unexpected(
          Status::ColumnDecodeError("No more values to read"));
    }
    T value;
    std::memcpy(&value, data_.data() + pos_, sizeof(T));
    pos_ += sizeof(T);
    ++values_read_;
    return value;
  }

  /// @brief Reads a batch of values into a span.
  /// @param values The output span to fill.
  /// @return The number of values read.
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

  /// @brief Sets the total number of boolean values to read.
  void SetNumValues(size_t num_values) { num_values_ = num_values; }

  [[nodiscard]] size_t ValuesRemaining() const override {
    return num_values_ - values_read_;
  }

  /// @brief Reads a single boolean value.
  /// @return The boolean value, or decode error if no more values.
  [[nodiscard]] Result<bool> ReadValue() {
    if (values_read_ >= num_values_) {
      return std::unexpected(
          Status::ColumnDecodeError("No more boolean values to read"));
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

  /// @brief Reads a single string value.
  /// @return The string, or decode error if insufficient data.
  [[nodiscard]] Result<std::string> ReadValue() {
    if (pos_ + 4 > data_.size()) {
      return std::unexpected(
          Status::ColumnDecodeError("Not enough data for string length"));
    }
    uint32_t length;
    std::memcpy(&length, data_.data() + pos_, 4);
    pos_ += 4;

    if (pos_ + length > data_.size()) {
      return std::unexpected(
          Status::ColumnDecodeError("Not enough data for string content"));
    }
    std::string value(reinterpret_cast<const char*>(data_.data() + pos_),
                      length);
    pos_ += length;
    return value;
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
  /// @brief Constructs a decoder with the given bit width.
  /// @param bit_width The number of bits per value.
  explicit RleBitPackingDecoder(int bit_width) : bit_width_(bit_width) {}

  /// @brief Sets the input data and expected value count.
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

  /// @brief Returns the number of values remaining.
  [[nodiscard]] size_t ValuesRemaining() const {
    return num_values_ - values_read_;
  }

  /// @brief Reads a single value.
  /// @return The value, or decode error.
  [[nodiscard]] Result<int32_t> ReadValue() {
    if (current_count_ == 0) {
      auto status = ReadNextGroup();
      if (!status.ok()) {
        return std::unexpected(status.error());
      }
    }

    int32_t value;
    if (is_rle_) {
      value = current_value_;
      --current_count_;
    } else {
      value = ReadBitPackedValue();
      --current_count_;
    }
    ++values_read_;
    return value;
  }

  /// @brief Reads a batch of values into a span.
  /// @param values The output span to fill.
  /// @return Status indicating success or decode error.
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

    uint32_t indicator = ReadVarInt();
    if (indicator & 1) {
      // Bit-packed run.
      is_rle_ = false;
      bit_packed_count_ = ((indicator >> 1) * 8);
      current_count_ = bit_packed_count_;
      bit_packed_pos_ = 0;
      size_t bytes_needed = (bit_packed_count_ * bit_width_ + 7) / 8;
      bit_packed_data_ = std::span(data_.data() + pos_, bytes_needed);
      pos_ += bytes_needed;
    } else {
      // RLE run.
      is_rle_ = true;
      current_count_ = indicator >> 1;
      current_value_ = ReadFixedWidth();
    }
    return Status::OK();
  }

  uint32_t ReadVarInt() {
    uint32_t result = 0;
    uint32_t shift = 0;
    while (pos_ < data_.size()) {
      uint8_t byte = data_[pos_++];
      result |= static_cast<uint32_t>(byte & 0x7F) << shift;
      if ((byte & 0x80) == 0) {
        break;
      }
      shift += 7;
    }
    return result;
  }

  int32_t ReadFixedWidth() {
    int32_t value = 0;
    size_t bytes_to_read = (bit_width_ + 7) / 8;
    for (size_t i = 0; i < bytes_to_read && pos_ < data_.size(); ++i) {
      value |= static_cast<int32_t>(data_[pos_++]) << (i * 8);
    }
    return value;
  }

  int32_t ReadBitPackedValue() {
    size_t bit_offset = bit_packed_pos_ * bit_width_;
    size_t byte_offset = bit_offset / 8;
    size_t bit_in_byte = bit_offset % 8;

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
// Dictionary Decoder
// =============================================================================

/// @brief Dictionary decoder for dictionary-encoded columns.
///
/// Dictionary encoding stores unique values in a dictionary and uses
/// RLE/bit-packed indices to reference them.
///
/// @tparam T The dictionary value type.
template <typename T>
class DictionaryDecoder {
 public:
  /// @brief Sets the dictionary values.
  void SetDictionary(std::vector<T> dictionary) {
    dictionary_ = std::move(dictionary);
  }

  /// @brief Sets the encoded indices data.
  /// @param data The RLE/bit-packed index data.
  /// @param bit_width The bit width for indices.
  /// @param num_values The number of values to decode.
  void SetIndices(std::span<const uint8_t> data, int bit_width,
                  size_t num_values) {
    index_decoder_ = std::make_unique<RleBitPackingDecoder>(bit_width);
    index_decoder_->SetData(data, num_values);
  }

  /// @brief Returns the number of values remaining.
  [[nodiscard]] size_t ValuesRemaining() const {
    return index_decoder_ ? index_decoder_->ValuesRemaining() : 0;
  }

  /// @brief Reads a single value from the dictionary.
  /// @return The value, or decode error.
  [[nodiscard]] Result<T> ReadValue() {
    if (!index_decoder_) {
      return std::unexpected(
          Status::ColumnDecodeError("Dictionary indices not set"));
    }
    auto index_result = index_decoder_->ReadValue();
    if (!index_result.has_value()) {
      return std::unexpected(index_result.error());
    }
    int32_t index = index_result.value();
    if (index < 0 || static_cast<size_t>(index) >= dictionary_.size()) {
      return std::unexpected(
          Status::OutOfRange("Dictionary index out of range"));
    }
    return dictionary_[index];
  }

  /// @brief Returns a reference to the dictionary.
  [[nodiscard]] const std::vector<T>& dictionary() const { return dictionary_; }

 private:
  std::vector<T> dictionary_;
  std::unique_ptr<RleBitPackingDecoder> index_decoder_;
};

// =============================================================================
// Utility Functions
// =============================================================================

/// @brief Computes the bit width needed to represent a maximum value.
/// @param max_value The maximum value to represent.
/// @return The minimum number of bits needed.
[[nodiscard]] inline int ComputeBitWidth(int max_value) {
  if (max_value <= 0) {
    return 0;
  }
  return std::bit_width(static_cast<unsigned>(max_value));
}

}  // namespace quest::datasource::parquet

#endif  // PARQUET_READER_ENCODING_HPP
