// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <compare>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quest::columnar {

enum class Repetition : int8_t { kRequired = 0, kOptional = 1, kRepeated = 2 };

/// @brief A `TypeId::kDate32` value: days since 1970-01-01, the Parquet DATE representation.
struct Date32 {
  std::int32_t days;
  auto operator<=>(const Date32&) const = default;
};
// Parquet plain/dictionary decoding memcpy's raw INT32 values into Date32.
static_assert(sizeof(Date32) == sizeof(std::int32_t) && std::is_trivially_copyable_v<Date32>);

/// @brief A `TypeId::kTimestamp` value: microseconds since 1970-01-01 00:00:00, no time zone.
struct Timestamp {
  std::int64_t micros;
  auto operator<=>(const Timestamp&) const = default;
};

/// @brief A `DataType` Decimal value: `unscaled / 10^scale`, with the scale in the DataType.
struct Decimal64 {
  std::int64_t unscaled;
  auto operator<=>(const Decimal64&) const = default;
};

/// @brief The kind of a column type.
///
/// The value is the spill format's type tag: append new kinds, never reorder. kInt8..kInt64
/// are in width order.
enum class TypeId : std::uint8_t {
  kBool,
  kInt8,
  kInt16,
  kInt32,
  kInt64,
  kFloat,
  kDouble,
  kString,
  kDate32,
  kTimestamp,
  kDecimal,
};

/// @brief The type of a column: a TypeId, plus the precision and scale of a Decimal.
struct DataType {
  /// @brief The widest Decimal: its unscaled values fit an int64.
  static constexpr std::uint8_t kMaxDecimalPrecision = 18;

  TypeId id = TypeId::kBool;
  std::uint8_t precision = 0;  ///< Decimal only: total digits, 1..18.
  std::uint8_t scale = 0;      ///< Decimal only: digits after the point, 0..precision.

  // One constant per TypeId except kDecimal, which needs a precision and scale.
  static const DataType kBool;
  static const DataType kInt8;
  static const DataType kInt16;
  static const DataType kInt32;
  static const DataType kInt64;
  static const DataType kFloat;
  static const DataType kDouble;
  static const DataType kString;
  static const DataType kDate32;
  static const DataType kTimestamp;

  [[nodiscard]] static constexpr DataType Decimal(std::uint8_t precision,
                                                  std::uint8_t scale) noexcept {
    return DataType{TypeId::kDecimal, precision, scale};
  }

  constexpr bool operator==(const DataType&) const = default;
};

inline constexpr DataType DataType::kBool{TypeId::kBool};
inline constexpr DataType DataType::kInt8{TypeId::kInt8};
inline constexpr DataType DataType::kInt16{TypeId::kInt16};
inline constexpr DataType DataType::kInt32{TypeId::kInt32};
inline constexpr DataType DataType::kInt64{TypeId::kInt64};
inline constexpr DataType DataType::kFloat{TypeId::kFloat};
inline constexpr DataType DataType::kDouble{TypeId::kDouble};
inline constexpr DataType DataType::kString{TypeId::kString};
inline constexpr DataType DataType::kDate32{TypeId::kDate32};
inline constexpr DataType DataType::kTimestamp{TypeId::kTimestamp};

/// @brief The C++ type a kernel reads for a TypeId (`CType`), and the type's name.
template <TypeId>
struct TypeTraits;

template <>
struct TypeTraits<TypeId::kBool> {
  using CType = bool;
  static constexpr std::string_view kName = "Bool";
};
template <>
struct TypeTraits<TypeId::kInt8> {
  using CType = std::int8_t;
  static constexpr std::string_view kName = "Int8";
};
template <>
struct TypeTraits<TypeId::kInt16> {
  using CType = std::int16_t;
  static constexpr std::string_view kName = "Int16";
};
template <>
struct TypeTraits<TypeId::kInt32> {
  using CType = std::int32_t;
  static constexpr std::string_view kName = "Int32";
};
template <>
struct TypeTraits<TypeId::kInt64> {
  using CType = std::int64_t;
  static constexpr std::string_view kName = "Int64";
};
template <>
struct TypeTraits<TypeId::kFloat> {
  using CType = float;
  static constexpr std::string_view kName = "Float";
};
template <>
struct TypeTraits<TypeId::kDouble> {
  using CType = double;
  static constexpr std::string_view kName = "Double";
};
template <>
struct TypeTraits<TypeId::kString> {
  using CType = std::string_view;
  static constexpr std::string_view kName = "String";
};
template <>
struct TypeTraits<TypeId::kDate32> {
  using CType = Date32;
  static constexpr std::string_view kName = "Date32";
};
template <>
struct TypeTraits<TypeId::kTimestamp> {
  using CType = Timestamp;
  static constexpr std::string_view kName = "Timestamp";
};
template <>
struct TypeTraits<TypeId::kDecimal> {
  using CType = Decimal64;
  static constexpr std::string_view kName = "Decimal";
};

/// @brief What `VisitType` passes to its callable: the traits of one TypeId.
template <TypeId Id>
struct TypeTag : TypeTraits<Id> {
  static constexpr TypeId kId = Id;
};

/// @brief Calls @p f with the `TypeTag` of @p type's id; the one switch over types.
///
/// Write @p f as `[&]<typename Tag>(Tag) { using T = typename Tag::CType; ... }`. Every
/// branch must return the same type.
template <typename F>
constexpr decltype(auto) VisitType(DataType type, F&& f) {
  switch (type.id) {
    case TypeId::kBool:
      return std::forward<F>(f)(TypeTag<TypeId::kBool>{});
    case TypeId::kInt8:
      return std::forward<F>(f)(TypeTag<TypeId::kInt8>{});
    case TypeId::kInt16:
      return std::forward<F>(f)(TypeTag<TypeId::kInt16>{});
    case TypeId::kInt32:
      return std::forward<F>(f)(TypeTag<TypeId::kInt32>{});
    case TypeId::kInt64:
      return std::forward<F>(f)(TypeTag<TypeId::kInt64>{});
    case TypeId::kFloat:
      return std::forward<F>(f)(TypeTag<TypeId::kFloat>{});
    case TypeId::kDouble:
      return std::forward<F>(f)(TypeTag<TypeId::kDouble>{});
    case TypeId::kString:
      return std::forward<F>(f)(TypeTag<TypeId::kString>{});
    case TypeId::kDate32:
      return std::forward<F>(f)(TypeTag<TypeId::kDate32>{});
    case TypeId::kTimestamp:
      return std::forward<F>(f)(TypeTag<TypeId::kTimestamp>{});
    case TypeId::kDecimal:
      return std::forward<F>(f)(TypeTag<TypeId::kDecimal>{});
  }
  std::unreachable();
}

[[nodiscard]] constexpr bool IsInteger(DataType type) noexcept {
  return type.id >= TypeId::kInt8 && type.id <= TypeId::kInt64;
}

[[nodiscard]] constexpr bool IsDecimal(DataType type) noexcept {
  return type.id == TypeId::kDecimal;
}

/// @brief Integers, Float, Double and Decimal.
[[nodiscard]] constexpr bool IsNumeric(DataType type) noexcept {
  return IsInteger(type) || type.id == TypeId::kFloat || type.id == TypeId::kDouble ||
         IsDecimal(type);
}

[[nodiscard]] constexpr bool IsTemporal(DataType type) noexcept {
  return type.id == TypeId::kDate32 || type.id == TypeId::kTimestamp;
}

/// @brief "Int32", or "Decimal(15,2)".
[[nodiscard]] inline std::string GetTypeName(DataType type) {
  if (IsDecimal(type)) {
    return std::format("Decimal({},{})", type.precision, type.scale);
  }
  return VisitType(type, []<typename Tag>(Tag) { return std::string(Tag::kName); });
}

/// @brief 10^@p exponent for @p exponent in [0, 18].
[[nodiscard]] constexpr std::int64_t PowerOfTen(int exponent) noexcept {
  std::int64_t value = 1;
  for (int i = 0; i < exponent; ++i) {
    value *= 10;
  }
  return value;
}

/// @brief @p unscaled / 10^@p scale as text with exactly @p scale decimals, as DuckDB prints.
[[nodiscard]] inline std::string FormatDecimal(std::int64_t unscaled, int scale) {
  // Through uint64: -INT64_MIN does not fit an int64.
  const bool negative = unscaled < 0;
  const auto magnitude = negative ? std::uint64_t{0} - static_cast<std::uint64_t>(unscaled)
                                  : static_cast<std::uint64_t>(unscaled);
  const auto divisor = static_cast<std::uint64_t>(PowerOfTen(scale));
  std::string text = std::format("{}{}", negative ? "-" : "", magnitude / divisor);
  if (scale > 0) {
    text += std::format(".{:0{}}", magnitude % divisor, scale);
  }
  return text;
}

}  // namespace quest::columnar
