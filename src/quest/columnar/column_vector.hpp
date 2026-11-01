// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <chrono>
#include <concepts>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <type_traits>
#include <vector>

#include "quest/columnar/types.hpp"

namespace quest::columnar {

__extension__ using Int128 = __int128;  // __extension__: -Wpedantic rejects bare __int128

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

/// @brief "1994-01-01".
[[nodiscard]] inline std::string FormatDate32(Date32 date) {
  const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{date.days}}};
  return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                     static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
}

/// @brief DuckDB's text form: fractional seconds only when nonzero, without trailing zeros.
[[nodiscard]] inline std::string FormatTimestamp(Timestamp timestamp) {
  using namespace std::chrono;
  const sys_time<microseconds> time{microseconds{timestamp.micros}};
  const auto day = floor<days>(time);
  const year_month_day ymd{day};
  const hh_mm_ss clock{time - day};
  std::string text = std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", static_cast<int>(ymd.year()),
                                 static_cast<unsigned>(ymd.month()),
                                 static_cast<unsigned>(ymd.day()), clock.hours().count(),
                                 clock.minutes().count(), clock.seconds().count());
  if (const auto micros = clock.subseconds().count(); micros != 0) {
    std::string fraction = std::format("{:06}", micros);
    fraction.erase(fraction.find_last_not_of('0') + 1);
    text += "." + fraction;
  }
  return text;
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

// =============================================================================
// FormatCell and Scalar
// =============================================================================

/// @brief Row @p row of @p column as text: "null", true, 42, 1.500000, text, 1994-01-01,
/// 1994-01-01 12:00:00.5, 1234.56.
[[nodiscard]] inline std::string FormatCell(const ColumnVector& column, std::size_t row) {
  if (column.IsNull(row)) {
    return "null";
  }
  return VisitType(column.GetType(), [&]<typename Tag>(Tag) -> std::string {
    using T = typename Tag::CType;
    const T value = column.Get<T>(row);
    if constexpr (std::same_as<T, bool>) {
      return value ? "true" : "false";
    } else if constexpr (std::same_as<T, std::string_view>) {
      return std::string(value);
    } else if constexpr (std::same_as<T, Date32>) {
      return FormatDate32(value);
    } else if constexpr (std::same_as<T, Timestamp>) {
      return FormatTimestamp(value);
    } else if constexpr (std::same_as<T, Decimal64>) {
      return FormatDecimal(value.unscaled, column.GetType().scale);
    } else {
      return std::to_string(value);
    }
  });
}

/// @brief One value of one type, or a typed null: a literal.
///
/// It holds a one-row column, so a literal broadcasts with `ColumnVector::Constant` and
/// prints with `FormatCell`.
class Scalar {
 public:
  /// @brief The NULL literal. It has no type of its own: it reads as String, and an operator
  /// gives it the other operand's type.
  Scalar() : Scalar(Null(DataType::kString)) {}
  explicit Scalar(bool value) : Scalar(DataType::kBool, value) {}
  explicit Scalar(std::int8_t value) : Scalar(DataType::kInt8, value) {}
  explicit Scalar(std::int16_t value) : Scalar(DataType::kInt16, value) {}
  explicit Scalar(std::int32_t value) : Scalar(DataType::kInt32, value) {}
  explicit Scalar(std::int64_t value) : Scalar(DataType::kInt64, value) {}
  explicit Scalar(float value) : Scalar(DataType::kFloat, value) {}
  explicit Scalar(double value) : Scalar(DataType::kDouble, value) {}
  explicit Scalar(std::string_view value) : Scalar(DataType::kString, value) {}
  // Without these, a string literal would pick the bool overload.
  explicit Scalar(const char* value) : Scalar(std::string_view(value)) {}
  explicit Scalar(const std::string& value) : Scalar(std::string_view(value)) {}
  explicit Scalar(Date32 value) : Scalar(DataType::kDate32, value) {}
  explicit Scalar(Timestamp value) : Scalar(DataType::kTimestamp, value) {}
  /// @brief Any other integer type as Int64 and any other float as Double, as `Lit(5u)` and
  /// `Lit(5L)` expect. A 64-bit unsigned value does not fit and stays a compile error.
  template <typename T>
    requires((std::is_integral_v<T> && !std::same_as<T, bool> &&
              !(std::is_unsigned_v<T> && sizeof(T) == 8)) ||
             std::is_floating_point_v<T>)
  explicit Scalar(T value)
      : Scalar(std::is_integral_v<T> ? DataType::kInt64 : DataType::kDouble,
               std::conditional_t<std::is_integral_v<T>, std::int64_t, double>(value)) {}

  /// @brief `unscaled / 10^scale`, typed as DuckDB types the literal: 0.05 is Decimal(3, 2).
  [[nodiscard]] static Scalar Decimal(std::int64_t unscaled, std::uint8_t scale) {
    int digits = 1;
    for (auto rest = unscaled; rest >= 10 || rest <= -10; rest /= 10) {
      ++digits;
    }
    const auto precision = static_cast<std::uint8_t>(
        std::min<int>(DataType::kMaxDecimalPrecision, std::max<int>(digits, scale + 1)));
    return Scalar(DataType::Decimal(precision, scale), Decimal64{unscaled});
  }

  /// @brief A null of @p type.
  [[nodiscard]] static Scalar Null(DataType type) {
    return VisitType(type, [&]<typename Tag>(Tag) {
      ColumnBuilder<typename Tag::CType> builder(type, 1);
      builder.AppendNull();
      return Scalar(std::move(builder).Finish());
    });
  }

  [[nodiscard]] DataType type() const noexcept { return column_->GetType(); }
  [[nodiscard]] bool is_null() const noexcept { return column_->IsNull(0); }
  /// @brief The value; @p T must be the CType of `type()`.
  template <typename T>
  [[nodiscard]] T Get() const noexcept {
    return column_->Get<T>(0);
  }
  /// @brief The one-row column.
  [[nodiscard]] const ColumnVector& column() const noexcept { return *column_; }

  /// @brief As SQL text: null, true, 42, "text", DATE '1994-01-01', TIMESTAMP '...', 0.05.
  [[nodiscard]] std::string ToSql() const {
    if (is_null()) {
      return "null";
    }
    switch (type().id) {
      case TypeId::kString:
        return "\"" + FormatCell(*column_, 0) + "\"";
      case TypeId::kDate32:
        return "DATE '" + FormatCell(*column_, 0) + "'";
      case TypeId::kTimestamp:
        return "TIMESTAMP '" + FormatCell(*column_, 0) + "'";
      default:
        return FormatCell(*column_, 0);
    }
  }

 private:
  template <typename T>
  Scalar(DataType type, T value) {
    ColumnBuilder<T> builder(type, 1);
    builder.Append(value);
    column_ = std::move(builder).Finish();
  }
  explicit Scalar(ColumnVectorPtr column) : column_(std::move(column)) {}

  ColumnVectorPtr column_;
};

}  // namespace quest::columnar
