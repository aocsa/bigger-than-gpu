// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include "quest/columnar/types.hpp"
#include "quest/common/result.hpp"

namespace quest::columnar {

using Value = std::variant<std::monostate,  // null
                           bool, int8_t, int16_t, int32_t, int64_t, float,
                           double, std::string, std::vector<uint8_t>, Date32, Timestamp>;

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
        } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
          return std::string(reinterpret_cast<const char*>(v.data()), v.size());
        } else if constexpr (std::is_same_v<T, Date32>) {
          const std::chrono::year_month_day ymd{std::chrono::sys_days{std::chrono::days{v.days}}};
          return std::format("{:04}-{:02}-{:02}", static_cast<int>(ymd.year()),
                             static_cast<unsigned>(ymd.month()), static_cast<unsigned>(ymd.day()));
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

class ColumnVector {
 public:
  virtual ~ColumnVector() = default;

  virtual DataType GetType() const = 0;
  virtual Value GetValue(size_t index) const = 0;
  virtual bool IsNull(size_t index) const = 0;
  virtual size_t size() const = 0;
};

template <typename T>
class TypedColumnVector : public  ColumnVector {
 public:
  TypedColumnVector(DataType type, size_t capacity)
      : type_(type), values_(capacity), null_bitmap_() {
    null_bitmap_.resize((capacity + 7) / 8, 0xFF);  // All valid by default
  }

  DataType GetType() const override { return type_; }

  size_t size() const override { return size_; }

  bool IsNull(size_t index) const override {
    if (index >= size_) return true;
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    return (null_bitmap_[byte_idx] & (1 << bit_idx)) == 0;
  }

  Value GetValue(size_t index) const override {
    if (IsNull(index)) return std::monostate{};
    return values_[index];
  }

  T GetTypedValue(size_t index) const {
    assert(index < size_ && "index out of range");
    return values_[index];
  }

  void Append(T value) {
    if (size_ >= values_.size()) {
      values_.resize(values_.size() * 2 + 1);
      null_bitmap_.resize((values_.size() + 7) / 8, 0xFF);
    }
    values_[size_] = value;
    SetValid(size_);
    ++size_;
  }

  void AppendNull() {
    if (size_ >= values_.size()) {
      values_.resize(values_.size() * 2 + 1);
      null_bitmap_.resize((values_.size() + 7) / 8, 0xFF);
    }
    values_[size_] = T{};
    SetNull(size_);
    ++size_;
  }

  void SetValue(size_t index, T value) {
    values_[index] = value;
    SetValid(index);
  }

  void SetNull(size_t index) {
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    null_bitmap_[byte_idx] &= ~(1 << bit_idx);
  }

  void SetValid(size_t index) {
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    null_bitmap_[byte_idx] |= (1 << bit_idx);
  }

  void SetSize(size_t new_size) { size_ = new_size; }

  std::span<T> GetMutableValues() { return std::span(values_.data(), size_); }
  std::span<const T> GetValues() const {
    return std::span(values_.data(), size_);
  }

  std::span<uint8_t> GetMutableNullBitmap() { return std::span(null_bitmap_); }

 private:
  DataType type_;
  std::vector<T> values_;
  std::vector<uint8_t> null_bitmap_;
  size_t size_ = 0;
};

class StringColumnVector : public ColumnVector {
 public:
  explicit StringColumnVector(DataType type, size_t capacity = 0)
      : type_(type) {
    values_.reserve(capacity);
    null_bitmap_.resize((capacity + 7) / 8, 0xFF);
  }

  DataType GetType() const override { return type_; }

  size_t size() const override { return values_.size(); }

  bool IsNull(size_t index) const override {
    if (index >= values_.size()) return true;
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    return (null_bitmap_[byte_idx] & (1 << bit_idx)) == 0;
  }

  Value GetValue(size_t index) const override {
    if (IsNull(index)) return std::monostate{};
    return values_[index];
  }

  const std::string& GetString(size_t index) const { return values_[index]; }

  void Append(std::string value) {
    EnsureCapacity();
    values_.push_back(std::move(value));
    SetValid(values_.size() - 1);
  }

  void AppendNull() {
    EnsureCapacity();
    values_.emplace_back();
    SetNull(values_.size() - 1);
  }

 private:
  void EnsureCapacity() {
    if (values_.size() >= null_bitmap_.size() * 8) {
      null_bitmap_.resize(null_bitmap_.size() + 8, 0xFF);
    }
  }

  void SetNull(size_t index) {
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    null_bitmap_[byte_idx] &= ~(1 << bit_idx);
  }

  void SetValid(size_t index) {
    size_t byte_idx = index / 8;
    size_t bit_idx = index % 8;
    null_bitmap_[byte_idx] |= (1 << bit_idx);
  }

  DataType type_;
  std::vector<std::string> values_;
  std::vector<uint8_t> null_bitmap_;
};

/// @brief The column class that stores C++ type @p T (`TypeTraits` CType).
template <typename T>
using ColumnOf =
    std::conditional_t<std::same_as<T, std::string_view>, StringColumnVector, TypedColumnVector<T>>;

/// @brief The `Value` alternative for C++ type @p T: a String cell is a `std::string`.
template <typename T>
using ValueOf = std::conditional_t<std::same_as<T, std::string_view>, std::string, T>;

inline std::unique_ptr<ColumnVector> CreateColumnVector(DataType type, size_t capacity) {
  return VisitType(type, [&]<typename Tag>(Tag) -> std::unique_ptr<ColumnVector> {
    return std::make_unique<ColumnOf<typename Tag::CType>>(type, capacity);
  });
}

/// @brief Append values from @p src onto a destination column of the same type.
[[nodiscard]] inline Status AppendColumnValues(ColumnVector& dest, const ColumnVector& src) {
  return VisitType(src.GetType(), [&]<typename Tag>(Tag) -> Status {
    using T = typename Tag::CType;
    auto* typed = dynamic_cast<ColumnOf<T>*>(&dest);
    if (typed == nullptr) {
      return Error::Invalid("Concatenate destination type mismatch");
    }
    for (size_t i = 0; i < src.size(); ++i) {
      const Value val = src.GetValue(i);
      if (std::holds_alternative<std::monostate>(val)) {
        typed->AppendNull();
      } else if (const auto* v = std::get_if<ValueOf<T>>(&val)) {
        typed->Append(*v);
      } else {
        return Error::Invalid("Concatenate value type mismatch (" + std::string(Tag::kName) + ")");
      }
    }
    return {};
  });
}

}  // namespace quest::columnar
