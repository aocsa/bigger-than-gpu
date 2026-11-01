// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <variant>

namespace quest::columnar {

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

enum class Repetition : int8_t { kRequired = 0, kOptional = 1, kRepeated = 2 };

// Arrow-compatible data types for our query engine
struct BoolType {
  static constexpr const char* name = "Bool";
};

struct Int8Type {
  static constexpr const char* name = "Int8";
};

struct Int16Type {
  static constexpr const char* name = "Int16";
};

struct Int32Type {
  static constexpr const char* name = "Int32";
};

struct Int64Type {
  static constexpr const char* name = "Int64";
};

struct FloatType {
  static constexpr const char* name = "Float";
};

struct DoubleType {
  static constexpr const char* name = "Double";
};

struct StringType {
  static constexpr const char* name = "String";
};

struct BinaryType {
  static constexpr const char* name = "Binary";
};

struct Int96Type {
  static constexpr const char* name = "Int96";
};

struct FixedLenByteArrayType {
  int32_t byte_width;
  static constexpr const char* name = "FixedLenByteArray";
};

using DataType = std::variant<BoolType, Int8Type, Int16Type, Int32Type, Int64Type,
                              FloatType, DoubleType, StringType, BinaryType,
                              Int96Type, FixedLenByteArrayType>;

inline DataType PhysicalTypeToDataType(PhysicalType type,
                                       int32_t type_length = 0) {
  switch (type) {
    case PhysicalType::kBoolean:
      return BoolType{};
    case PhysicalType::kInt32:
      return Int32Type{};
    case PhysicalType::kInt64:
      return Int64Type{};
    case PhysicalType::kInt96:
      return Int96Type{};
    case PhysicalType::kFloat:
      return FloatType{};
    case PhysicalType::kDouble:
      return DoubleType{};
    case PhysicalType::kByteArray:
      return BinaryType{};
    case PhysicalType::kFixedLenByteArray:
      return FixedLenByteArrayType{type_length};
  }
  return BinaryType{};  // Default fallback
}

inline std::string GetTypeName(const DataType& type) {
  return std::visit(
      [](const auto& t) -> std::string {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, FixedLenByteArrayType>) {
          return std::format("FixedLenByteArray({})", t.byte_width);
        } else {
          return T::name;
        }
      },
      type);
}

inline int32_t GetTypeByteWidth(const DataType& type) {
  return std::visit(
      [](const auto& t) -> int32_t {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, BoolType>) {
          return 1;
        } else if constexpr (std::is_same_v<T, Int8Type>) {
          return 1;
        } else if constexpr (std::is_same_v<T, Int16Type>) {
          return 2;
        } else if constexpr (std::is_same_v<T, Int32Type>) {
          return 4;
        } else if constexpr (std::is_same_v<T, Int64Type>) {
          return 8;
        } else if constexpr (std::is_same_v<T, FloatType>) {
          return 4;
        } else if constexpr (std::is_same_v<T, DoubleType>) {
          return 8;
        } else if constexpr (std::is_same_v<T, Int96Type>) {
          return 12;
        } else if constexpr (std::is_same_v<T, FixedLenByteArrayType>) {
          return t.byte_width;
        } else {
          return -1;  // Variable width
        }
      },
      type);
}

inline bool IsFixedWidth(const DataType& type) {
  return GetTypeByteWidth(type) > 0;
}

}  // namespace quest::columnar
