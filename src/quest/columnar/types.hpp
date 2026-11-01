// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <compare>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quest::columnar {

enum class Repetition : int8_t { kRequired = 0, kOptional = 1, kRepeated = 2 };

/// @brief A `DataType::kDate32` value: days since 1970-01-01, the Parquet DATE representation.
struct Date32 {
  std::int32_t days;
  auto operator<=>(const Date32&) const = default;
};
// Parquet plain/dictionary decoding memcpy's raw INT32 values into Date32.
static_assert(sizeof(Date32) == sizeof(std::int32_t) && std::is_trivially_copyable_v<Date32>);

/// @brief The type of a column.
///
/// The value is the spill format's type tag: append new types, never reorder. kInt8..kInt64
/// are in width order, and kInt8..kDouble are exactly the numeric types.
enum class DataType : std::uint8_t {
  kBool,
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kFloat,
  kDouble,
  kString,
  kDate32,
};

/// @brief The C++ type a kernel reads for a DataType (`CType`), and the type's name.
template <DataType>
struct TypeTraits;

template <>
struct TypeTraits<DataType::kBool> {
  using CType = bool;
  static constexpr std::string_view kName = "Bool";
};
template <>
struct TypeTraits<DataType::kInt8> {
  using CType = std::int8_t;
  static constexpr std::string_view kName = "Int8";
};
template <>
struct TypeTraits<DataType::kInt16> {
  using CType = std::int16_t;
  static constexpr std::string_view kName = "Int16";
};
template <>
struct TypeTraits<DataType::kInt32> {
  using CType = std::int32_t;
  static constexpr std::string_view kName = "Int32";
};
template <>
struct TypeTraits<DataType::kInt64> {
  using CType = std::int64_t;
  static constexpr std::string_view kName = "Int64";
};
template <>
struct TypeTraits<DataType::kFloat> {
  using CType = float;
  static constexpr std::string_view kName = "Float";
};
template <>
struct TypeTraits<DataType::kDouble> {
  using CType = double;
  static constexpr std::string_view kName = "Double";
};
template <>
struct TypeTraits<DataType::kString> {
  using CType = std::string_view;
  static constexpr std::string_view kName = "String";
};
template <>
struct TypeTraits<DataType::kDate32> {
  using CType = Date32;
  static constexpr std::string_view kName = "Date32";
};

/// @brief What `VisitType` passes to its callable: the traits of one DataType.
template <DataType Id>
struct TypeTag : TypeTraits<Id> {
  static constexpr DataType kId = Id;
};

/// @brief Calls @p f with the `TypeTag` of @p type; the one switch over DataType.
///
/// Write @p f as `[&]<typename Tag>(Tag) { using T = typename Tag::CType; ... }`. Every
/// branch must return the same type.
template <typename F>
constexpr decltype(auto) VisitType(DataType type, F&& f) {
  switch (type) {
    case DataType::kBool:
      return std::forward<F>(f)(TypeTag<DataType::kBool>{});
    case DataType::kInt8:
      return std::forward<F>(f)(TypeTag<DataType::kInt8>{});
    case DataType::kInt16:
      return std::forward<F>(f)(TypeTag<DataType::kInt16>{});
    case DataType::kInt32:
      return std::forward<F>(f)(TypeTag<DataType::kInt32>{});
    case DataType::kInt64:
      return std::forward<F>(f)(TypeTag<DataType::kInt64>{});
    case DataType::kFloat:
      return std::forward<F>(f)(TypeTag<DataType::kFloat>{});
    case DataType::kDouble:
      return std::forward<F>(f)(TypeTag<DataType::kDouble>{});
    case DataType::kString:
      return std::forward<F>(f)(TypeTag<DataType::kString>{});
    case DataType::kDate32:
      return std::forward<F>(f)(TypeTag<DataType::kDate32>{});
  }
  std::unreachable();
}

[[nodiscard]] constexpr bool IsInteger(DataType type) noexcept {
  return type >= DataType::kInt8 && type <= DataType::kInt64;
}

[[nodiscard]] constexpr bool IsNumeric(DataType type) noexcept {
  return type >= DataType::kInt8 && type <= DataType::kDouble;
}

[[nodiscard]] inline std::string GetTypeName(DataType type) {
  return VisitType(type, []<typename Tag>(Tag) { return std::string(Tag::kName); });
}

}  // namespace quest::columnar
