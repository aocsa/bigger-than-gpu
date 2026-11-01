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

/// @brief A `Date32Type` value: days since 1970-01-01, the Parquet DATE representation.
struct Date32 {
  std::int32_t days;
  auto operator<=>(const Date32&) const = default;
};
// Parquet plain/dictionary decoding memcpy's raw INT32 values into Date32.
static_assert(sizeof(Date32) == sizeof(std::int32_t) && std::is_trivially_copyable_v<Date32>);

using Value = std::variant<std::monostate,  // null
                           bool, int8_t, int16_t, int32_t, int64_t, float,
                           double, std::string, std::vector<uint8_t>, Date32>;

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
      : type_(std::move(type)), values_(capacity), null_bitmap_() {
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
      : type_(std::move(type)) {
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

inline std::unique_ptr<ColumnVector> CreateColumnVector(const DataType& type,
                                                        size_t capacity) {
  return std::visit(
      [capacity](const auto& t) -> std::unique_ptr<ColumnVector> {
        using T = std::decay_t<decltype(t)>;
        if constexpr (std::is_same_v<T, BoolType>) {
          return std::make_unique<TypedColumnVector<bool>>(t, capacity);
        } else if constexpr (std::is_same_v<T, Int8Type>) {
          return std::make_unique<TypedColumnVector<int8_t>>(t, capacity);
        } else if constexpr (std::is_same_v<T, Int16Type>) {
          return std::make_unique<TypedColumnVector<int16_t>>(t, capacity);
        } else if constexpr (std::is_same_v<T, Int32Type>) {
          return std::make_unique<TypedColumnVector<int32_t>>(t, capacity);
        } else if constexpr (std::is_same_v<T, Int64Type>) {
          return std::make_unique<TypedColumnVector<int64_t>>(t, capacity);
        } else if constexpr (std::is_same_v<T, FloatType>) {
          return std::make_unique<TypedColumnVector<float>>(t, capacity);
        } else if constexpr (std::is_same_v<T, DoubleType>) {
          return std::make_unique<TypedColumnVector<double>>(t, capacity);
        } else if constexpr (std::is_same_v<T, Date32Type>) {
          return std::make_unique<TypedColumnVector<Date32>>(t, capacity);
        } else {
          return std::make_unique<StringColumnVector>(t, capacity);
        }
      },
      type);
}

/// @brief Append values from @p src onto a typed destination column.
[[nodiscard]] inline Status AppendColumnValues(ColumnVector& dest, const ColumnVector& src) {
  return std::visit(
      [&dest, &src](const auto& type_tag) -> Status {
        using TypeTag = std::decay_t<decltype(type_tag)>;
        auto append_one = [&](auto* typed) -> Status {
          if (!typed) {
            return Error::Invalid("Concatenate destination type mismatch");
          }
          for (size_t i = 0; i < src.size(); ++i) {
            auto val = src.GetValue(i);
            if (std::holds_alternative<std::monostate>(val)) {
              typed->AppendNull();
              continue;
            }
            if constexpr (std::is_same_v<TypeTag, BoolType>) {
              if (auto* v = std::get_if<bool>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (bool)");
              }
            } else if constexpr (std::is_same_v<TypeTag, Int8Type>) {
              if (auto* v = std::get_if<int8_t>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (int8)");
              }
            } else if constexpr (std::is_same_v<TypeTag, Int16Type>) {
              if (auto* v = std::get_if<int16_t>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (int16)");
              }
            } else if constexpr (std::is_same_v<TypeTag, Int32Type>) {
              if (auto* v = std::get_if<int32_t>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (int32)");
              }
            } else if constexpr (std::is_same_v<TypeTag, Int64Type>) {
              if (auto* v = std::get_if<int64_t>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (int64)");
              }
            } else if constexpr (std::is_same_v<TypeTag, FloatType>) {
              if (auto* v = std::get_if<float>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (float)");
              }
            } else if constexpr (std::is_same_v<TypeTag, DoubleType>) {
              if (auto* v = std::get_if<double>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (double)");
              }
            } else if constexpr (std::is_same_v<TypeTag, Date32Type>) {
              if (auto* v = std::get_if<Date32>(&val)) {
                typed->Append(*v);
              } else {
                return Error::Invalid("Concatenate value type mismatch (date32)");
              }
            } else {
              if (auto* v = std::get_if<std::string>(&val)) {
                typed->Append(*v);
              } else {
                typed->Append(ValueToString(val));
              }
            }
          }
          return {};
        };

        if constexpr (std::is_same_v<TypeTag, BoolType>) {
          return append_one(dynamic_cast<TypedColumnVector<bool>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, Int8Type>) {
          return append_one(dynamic_cast<TypedColumnVector<int8_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, Int16Type>) {
          return append_one(dynamic_cast<TypedColumnVector<int16_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, Int32Type>) {
          return append_one(dynamic_cast<TypedColumnVector<int32_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, Int64Type>) {
          return append_one(dynamic_cast<TypedColumnVector<int64_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, FloatType>) {
          return append_one(dynamic_cast<TypedColumnVector<float>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, DoubleType>) {
          return append_one(dynamic_cast<TypedColumnVector<double>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, Date32Type>) {
          return append_one(dynamic_cast<TypedColumnVector<Date32>*>(&dest));
        } else {
          return append_one(dynamic_cast<StringColumnVector*>(&dest));
        }
      },
      src.GetType());
}

}  // namespace quest::columnar
