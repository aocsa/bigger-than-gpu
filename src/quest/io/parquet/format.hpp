// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file format.hpp
/// @brief Parquet format enums and their mapping onto columnar types.

#include <cstdint>

#include "quest/columnar/types.hpp"

namespace quest::io::parquet {

using columnar::DataType;

// Parquet physical types as defined in the specification
enum class PhysicalType : int8_t {
  kBoolean = 0,
  kInt32 = 1,
  kInt64 = 2,
  kInt96 = 3,  // Deprecated, used for timestamps
  kFloat = 4,
  kDouble = 5,
  kByteArray = 6,
  kFixedLenByteArray = 7
};

enum class LogicalType : int8_t {
  kNone = 0,
  kString,
  kMap,
  kList,
  kEnum,
  kDecimal,
  kDate,
  kTime,
  kTimestamp,
  kInteger,
  kJson,
  kBson,
  kUuid
};

enum class Encoding : int8_t {
  kPlain = 0,
  kPlainDictionary = 2,
  kRle = 3,
  kBitPacked = 4,  // Deprecated
  kDeltaBinaryPacked = 5,
  kDeltaLengthByteArray = 6,
  kDeltaByteArray = 7,
  kRleDictionary = 8,
  kByteStreamSplit = 9
};

enum class PageType : int8_t {
  kDataPage = 0,
  kIndexPage = 1,
  kDictionaryPage = 2,
  kDataPageV2 = 3
};

enum class CompressionType : int8_t {
  kUncompressed = 0,
  kSnappy = 1,
  kGzip = 2,
  kLzo = 3,
  kBrotli = 4,
  kLz4 = 5,
  kZstd = 6,
  kLz4Raw = 7
};

/// @brief The DataType a physical type reads as when no logical type says otherwise.
/// BYTE_ARRAY, INT96 and FIXED_LEN_BYTE_ARRAY read as String with the raw bytes.
[[nodiscard]] constexpr DataType PhysicalTypeToDataType(PhysicalType type) noexcept {
  switch (type) {
    case PhysicalType::kBoolean:
      return DataType::kBool;
    case PhysicalType::kInt32:
      return DataType::kInt32;
    case PhysicalType::kInt64:
      return DataType::kInt64;
    case PhysicalType::kFloat:
      return DataType::kFloat;
    case PhysicalType::kDouble:
      return DataType::kDouble;
    case PhysicalType::kInt96:
    case PhysicalType::kByteArray:
    case PhysicalType::kFixedLenByteArray:
      return DataType::kString;
  }
  return DataType::kString;
}

}  // namespace quest::io::parquet
