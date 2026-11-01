// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <algorithm>
#include <bit>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>
#include <variant>
#include <vector>

#include "quest/columnar/types.hpp"
#include "quest/common/result.hpp"

namespace quest::columnar {

/// @brief A Decimal cell as a `Value`: the unscaled value and its column's scale. Values
/// compare by member, so only values of one scale (one column) compare correctly.
struct DecimalValue {
  std::int64_t unscaled;
  std::uint8_t scale;
  auto operator<=>(const DecimalValue&) const = default;
};

using Value = std::variant<std::monostate,  // null
                           bool, int8_t, int16_t, int32_t, int64_t, float,
                           double, std::string, Date32, Timestamp, DecimalValue>;

/// @brief The type of a Decimal literal, as DuckDB types `0.05`: Decimal(3, 2).
[[nodiscard]] constexpr DataType DecimalValueType(const DecimalValue& value) noexcept {
  int digits = 1;
  for (auto magnitude = value.unscaled; magnitude >= 10 || magnitude <= -10; magnitude /= 10) {
    ++digits;
  }
  return DataType::Decimal(static_cast<std::uint8_t>(std::max(digits, value.scale + 1)),
                           value.scale);
}

__extension__ using Int128 = __int128;  // __extension__: -Wpedantic rejects bare __int128

/// @brief An integer or Decimal value's unscaled digits and scale (an integer has scale 0).
template <typename V>
  requires(std::is_integral_v<V> || std::same_as<V, DecimalValue>)
[[nodiscard]] constexpr std::pair<std::int64_t, int> UnscaledAndScale(const V& value) noexcept {
  if constexpr (std::same_as<V, DecimalValue>) {
    return {value.unscaled, value.scale};
  } else {
    return {static_cast<std::int64_t>(value), 0};
  }
}

/// @brief @p unscaled at @p from_scale rewritten at @p to_scale (>= from_scale), or nullopt
/// if it overflows an int64.
[[nodiscard]] inline std::optional<std::int64_t> Rescale(std::int64_t unscaled, int from_scale,
                                                         int to_scale) noexcept {
  std::int64_t out = 0;
  if (__builtin_mul_overflow(unscaled, PowerOfTen(to_scale - from_scale), &out)) {
    return std::nullopt;
  }
  return out;
}

/// @brief A numeric `Value` alternative as double; a Decimal divides by 10^scale.
template <typename V>
[[nodiscard]] double ToDoubleValue(const V& value) noexcept {
  if constexpr (std::same_as<V, DecimalValue>) {
    return static_cast<double>(value.unscaled) / static_cast<double>(PowerOfTen(value.scale));
  } else {
    return static_cast<double>(value);
  }
}

inline std::string ValueToString(const Value& value) {
  return std::visit(
      [](const auto& v) -> std::string {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return "null";
        } else if constexpr (std::is_same_v<T, bool>) {
          return v ? "true" : "false";
        } else if constexpr (std::is_same_v<T, std::string>) {
          return v;
        } else if constexpr (std::is_same_v<T, Date32>) {
          const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{v.days}}};
          return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                             static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
        } else if constexpr (std::is_same_v<T, DecimalValue>) {
          return FormatDecimal(v.unscaled, v.scale);
        } else if constexpr (std::is_same_v<T, Timestamp>) {
          // DuckDB's text form: fractional seconds only when nonzero, without trailing zeros.
          using namespace std::chrono;
          const sys_time<microseconds> time{microseconds{v.micros}};
          const auto day = floor<days>(time);
          const year_month_day ymd{day};
          const hh_mm_ss clock{time - day};
          std::string text = std::format(
              "{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(ymd.year()),
              static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()),
              clock.hours().count(), clock.minutes().count(), clock.seconds().count());
          if (const auto micros = clock.subseconds().count(); micros != 0) {
            std::string fraction = std::format("{:06}", micros);
            fraction.erase(fraction.find_last_not_of('0') + 1);
            text += "." + fraction;
          }
          return text;
        } else {
          return std::to_string(v);
        }
      },
      value);
}

/// @brief The `Value` alternative for C++ type @p T: a String cell is a `std::string` and a
/// Decimal cell a `DecimalValue`.
template <typename T>
using ValueOf = std::conditional_t<
    std::same_as<T, std::string_view>, std::string,
    std::conditional_t<std::same_as<T, Decimal64>, DecimalValue, T>>;

/// @brief @p value as C++ type @p T; a `DecimalValue` drops its scale.
template <typename T>
[[nodiscard]] T FromValueOf(const ValueOf<T>& value) {
  if constexpr (std::same_as<T, Decimal64>) {
    return Decimal64{value.unscaled};
  } else {
    return T(value);
  }
}

/// @brief How a column stores C++ type @p T: `std::vector<bool>` packs bits and has no
/// `data()`, so a Bool is one byte.
template <typename T>
using StorageOf = std::conditional_t<std::same_as<T, bool>, std::uint8_t, T>;

// =============================================================================
// Buffer
// =============================================================================

/// @brief Read-only bytes that columns share; `owner` keeps the container alive.
struct Buffer {
  std::shared_ptr<const void> owner;
  std::span<const std::byte> bytes;

  /// @brief Moves @p container (a vector or string) behind a shared owner.
  template <typename Container>
  [[nodiscard]] static Buffer From(Container&& container) {
    auto owned = std::make_shared<const std::decay_t<Container>>(std::forward<Container>(container));
    const auto bytes = std::as_bytes(std::span(*owned));
    return Buffer{std::move(owned), bytes};
  }

  /// @brief The bytes as @p T; the buffer must have been made from a container of @p T.
  template <typename T>
  [[nodiscard]] const T* As() const noexcept {
    return reinterpret_cast<const T*>(bytes.data());
  }
};

// =============================================================================
// ColumnView
// =============================================================================

/// @brief Typed, non-owning reads of a column; kernels read through this.
///
/// @tparam T The column's `TypeTraits` CType (`std::string_view` for String). A constant
/// column (a broadcast literal) reads its one row for every row: its stride is 0.
template <typename T>
class ColumnView {
 public:
  [[nodiscard]] T operator[](std::size_t i) const noexcept {
    const std::size_t row = i * stride_;
    if constexpr (std::same_as<T, std::string_view>) {
      return {chars_ + offsets_[row], static_cast<std::size_t>(offsets_[row + 1] - offsets_[row])};
    } else if constexpr (std::same_as<T, bool>) {
      return static_cast<const std::uint8_t*>(values_)[row] != 0;
    } else {
      return static_cast<const T*>(values_)[row];
    }
  }

  [[nodiscard]] bool IsNull(std::size_t i) const noexcept {
    const std::size_t row = i * stride_;
    return validity_ != nullptr && ((validity_[row >> 3] >> (row & 7)) & 1) == 0;
  }

  /// @brief False when no row is null; kernels can then skip `IsNull`.
  [[nodiscard]] bool has_nulls() const noexcept { return validity_ != nullptr; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }

 private:
  friend class ColumnVector;

  const void* values_ = nullptr;           // StorageOf<T>; unused for String
  const std::int64_t* offsets_ = nullptr;  // String only: size + 1 offsets into chars_
  const char* chars_ = nullptr;            // String only
  const std::uint8_t* validity_ = nullptr; // bit i set = row i valid; null when no nulls
  std::size_t size_ = 0;
  std::size_t stride_ = 1;
};

template <typename T>
class ColumnBuilder;
class RecordBatchSerializer;

// =============================================================================
// ColumnVector
// =============================================================================

class ColumnVector;
using ColumnVectorPtr = std::shared_ptr<const ColumnVector>;

/// @brief An immutable column: Arrow-style buffers that batches share without copies.
///
/// Fixed-width types keep their values in one buffer; String keeps `size + 1` int64 offsets
/// and one chars buffer. The validity bitmap (bit i set = row i valid) exists only when a
/// row is null. Only a `ColumnBuilder` or `Constant` makes one.
class ColumnVector final {
  struct Key {
    explicit Key() = default;
  };

 public:
  /// @brief For `ColumnBuilder` and `Constant`; @p Key keeps it out of other code.
  ColumnVector(Key /*key*/, DataType type, std::size_t size, std::size_t null_count,
               bool constant, Buffer validity, Buffer values, Buffer offsets)
      : type_(type),
        size_(size),
        null_count_(null_count),
        constant_(constant),
        validity_(std::move(validity)),
        values_(std::move(values)),
        offsets_(std::move(offsets)) {}

  /// @brief @p one_row broadcast to @p size rows, sharing its buffers (a literal).
  [[nodiscard]] static ColumnVectorPtr Constant(const ColumnVector& one_row, std::size_t size) {
    assert(one_row.size_ == 1 && "a constant column repeats one row");
    return std::make_shared<const ColumnVector>(Key{}, one_row.type_, size,
                                                one_row.null_count_ == 0 ? 0 : size, true,
                                                one_row.validity_, one_row.values_,
                                                one_row.offsets_);
  }

  [[nodiscard]] DataType GetType() const noexcept { return type_; }
  [[nodiscard]] std::size_t size() const noexcept { return size_; }
  [[nodiscard]] std::size_t null_count() const noexcept { return null_count_; }
  /// @brief True for a broadcast literal: one stored row stands for every row.
  [[nodiscard]] bool is_constant() const noexcept { return constant_; }

  [[nodiscard]] bool IsNull(std::size_t i) const noexcept {
    // The bitmap, not null_count_: a null constant of 0 rows has no nulls but is still null.
    if (validity_.bytes.empty()) {
      return false;
    }
    const std::size_t row = constant_ ? 0 : i;
    const auto* bits = validity_.As<std::uint8_t>();
    return ((bits[row >> 3] >> (row & 7)) & 1) == 0;
  }

  /// @brief Typed reads; @p T must be the CType of `GetType()`.
  template <typename T>
  [[nodiscard]] ColumnView<T> View() const noexcept {
    assert(VisitType(type_, []<typename Tag>(Tag) {
             return std::same_as<typename Tag::CType, T>;
           }) && "View<T> of a column of another type");
    ColumnView<T> view;
    view.size_ = size_;
    view.stride_ = constant_ ? 0 : 1;
    view.validity_ = validity_.bytes.empty() ? nullptr : validity_.As<std::uint8_t>();
    if constexpr (std::same_as<T, std::string_view>) {
      view.offsets_ = offsets_.As<std::int64_t>();
      view.chars_ = values_.As<char>();
    } else {
      view.values_ = values_.As<StorageOf<T>>();
    }
    return view;
  }

  /// @brief The value at row @p i; @p T must be the CType of `GetType()`.
  template <typename T>
  [[nodiscard]] T Get(std::size_t i) const noexcept {
    return View<T>()[i];
  }

  /// @brief Row @p i as a `Value` (null, or past the end: monostate). Not for hot paths.
  [[nodiscard]] Value GetValue(std::size_t i) const {
    if (i >= size_ || IsNull(i)) {
      return std::monostate{};
    }
    return VisitType(type_, [&]<typename Tag>(Tag) -> Value {
      using T = typename Tag::CType;
      if constexpr (std::same_as<T, Decimal64>) {
        return DecimalValue{Get<T>(i).unscaled, type_.scale};
      } else {
        return ValueOf<T>(Get<T>(i));
      }
    });
  }

  /// @brief Bytes in the column's buffers; a constant column counts its one row.
  [[nodiscard]] std::size_t size_in_bytes() const noexcept {
    return validity_.bytes.size() + values_.bytes.size() + offsets_.bytes.size();
  }

 private:
  template <typename T>
  friend class ColumnBuilder;
  friend class RecordBatchSerializer;  // spill writes and reads the buffers as they are

  DataType type_;
  std::size_t size_ = 0;
  std::size_t null_count_ = 0;
  bool constant_ = false;
  Buffer validity_;  // empty when null_count_ == 0
  Buffer values_;    // fixed-width values, or the String chars
  Buffer offsets_;   // String only: size_ + 1 int64 offsets
};

// =============================================================================
// ColumnBuilder
// =============================================================================

/// @brief Appends values, then `Finish()` makes the read-only column.
/// @tparam T The column's `TypeTraits` CType (`std::string_view` for String).
template <typename T>
class ColumnBuilder {
 public:
  explicit ColumnBuilder(DataType type, std::size_t capacity = 0) : type_(type) {
    if constexpr (std::same_as<T, std::string_view>) {
      offsets_.reserve(capacity + 1);
    } else {
      values_.reserve(capacity);
    }
  }

  void Append(T value) {
    if constexpr (std::same_as<T, std::string_view>) {
      chars_.append(value);
      offsets_.push_back(static_cast<std::int64_t>(chars_.size()));
    } else {
      values_.push_back(static_cast<StorageOf<T>>(value));
    }
    AppendValidity(true);
  }

  /// @brief Appends a null; the first one makes the validity bitmap.
  void AppendNull() {
    if constexpr (std::same_as<T, std::string_view>) {
      offsets_.push_back(static_cast<std::int64_t>(chars_.size()));
    } else {
      values_.emplace_back();
    }
    AppendValidity(false);
  }

  /// @brief Appends every row of @p column, which has this builder's type: one copy of each
  /// buffer instead of one call for each row.
  void AppendColumn(const ColumnVector& column) {
    const std::size_t n = column.size_;
    if (column.constant_) {
      const auto view = column.View<T>();
      for (std::size_t i = 0; i < n; ++i) {
        view.IsNull(i) ? AppendNull() : Append(view[i]);
      }
      return;
    }
    if constexpr (std::same_as<T, std::string_view>) {
      const auto* offsets = column.offsets_.template As<std::int64_t>();
      const std::int64_t shift = static_cast<std::int64_t>(chars_.size()) - offsets[0];
      chars_.append(column.values_.template As<char>() + offsets[0],
                    static_cast<std::size_t>(offsets[n] - offsets[0]));
      for (std::size_t i = 1; i <= n; ++i) {
        offsets_.push_back(offsets[i] + shift);
      }
    } else {
      const auto* values = column.values_.template As<StorageOf<T>>();
      values_.insert(values_.end(), values, values + n);
    }
    if (column.validity_.bytes.empty() && null_count_ == 0) {
      size_ += n;  // no bitmap on either side
      return;
    }
    for (std::size_t i = 0; i < n; ++i) {
      AppendValidity(!column.IsNull(i));
    }
  }

  [[nodiscard]] std::size_t size() const noexcept { return size_; }

  /// @brief Moves the buffers into a read-only column; the builder is empty after.
  [[nodiscard]] ColumnVectorPtr Finish() && {
    Buffer validity = null_count_ == 0 ? Buffer{} : Buffer::From(std::move(validity_));
    Buffer values;
    Buffer offsets;
    if constexpr (std::same_as<T, std::string_view>) {
      values = Buffer::From(std::move(chars_));
      offsets = Buffer::From(std::move(offsets_));
    } else {
      values = Buffer::From(std::move(values_));
    }
    return std::make_shared<const ColumnVector>(ColumnVector::Key{}, type_, size_, null_count_,
                                                false, std::move(validity), std::move(values),
                                                std::move(offsets));
  }

 private:
  void AppendValidity(bool valid) {
    const std::size_t row = size_++;
    if (valid && null_count_ == 0) {
      return;  // no bitmap until the first null
    }
    if (null_count_ == 0) {
      validity_.assign(row / 8 + 1, 0xFF);  // every earlier row is valid
    } else if (row / 8 == validity_.size()) {
      validity_.push_back(0xFF);
    }
    if (!valid) {
      validity_[row / 8] &= static_cast<std::uint8_t>(~(1U << (row % 8)));
      ++null_count_;
    }
  }

  DataType type_;
  std::vector<StorageOf<T>> values_;        // fixed width only
  std::vector<std::int64_t> offsets_{0};    // String only
  std::string chars_;                       // String only
  std::vector<std::uint8_t> validity_;      // empty until the first null
  std::size_t size_ = 0;
  std::size_t null_count_ = 0;
};

/// @brief A column of @p type holding @p values (monostate is null), for code that still holds
/// `Value`s.
/// @return The column, or `TypeError` if a value is not of @p type.
[[nodiscard]] inline Result<ColumnVectorPtr> ColumnFromValues(DataType type,
                                                              std::span<const Value> values) {
  return VisitType(type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = typename Tag::CType;
    ColumnBuilder<T> builder(type, values.size());
    for (const Value& value : values) {
      if (std::holds_alternative<std::monostate>(value)) {
        builder.AppendNull();
      } else if (const auto* v = std::get_if<ValueOf<T>>(&value)) {
        if constexpr (std::same_as<T, Decimal64>) {
          if (v->scale != type.scale) {
            return Error::TypeError("Decimal value of scale " + std::to_string(v->scale) +
                                    " in a column of scale " + std::to_string(type.scale));
          }
        }
        builder.Append(FromValueOf<T>(*v));
      } else {
        return Error::TypeError("Value is not a " + std::string(Tag::kName));
      }
    }
    return std::move(builder).Finish();
  });
}

}  // namespace quest::columnar
