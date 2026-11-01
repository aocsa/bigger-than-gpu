// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstdint>
#include <format>
#include <string>
#include <variant>

namespace quest::columnar {

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

/// @brief Calendar date as days since 1970-01-01; values are `Date32`.
struct Date32Type {
  static constexpr const char* name = "Date32";
};

// The variant index is the spill format's type tag: append new types, never reorder.
using DataType = std::variant<BoolType, Int8Type, Int16Type, Int32Type, Int64Type,
                              FloatType, DoubleType, StringType, BinaryType,
                              Int96Type, FixedLenByteArrayType, Date32Type>;

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
        } else if constexpr (std::is_same_v<T, Date32Type>) {
          return 4;
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
