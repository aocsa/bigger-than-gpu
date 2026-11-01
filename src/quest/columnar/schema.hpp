// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <algorithm>
#include <cassert>
#include <format>
#include <optional>
#include <string>
#include <vector>

#include "quest/columnar/types.hpp"
#include "quest/common/result.hpp"

namespace quest::columnar {

struct ColumnStatistics {
  std::optional<std::vector<uint8_t>> min_value;
  std::optional<std::vector<uint8_t>> max_value;
  std::optional<int64_t> null_count;
  std::optional<int64_t> distinct_count;
  std::optional<int64_t> num_values;

  bool HasMinMax() const { return min_value.has_value() && max_value.has_value(); }
};

// Field definition (similar to Arrow Field)
struct Field {
  std::string name;
  DataType data_type;
  Repetition repetition = Repetition::kRequired;
  std::optional<int32_t> field_id;
  std::optional<int32_t> type_length;  // For fixed-length types
  /// Set for a Parquet INT32/INT64 DECIMAL column, which reads as Double: value / 10^scale.
  std::optional<int32_t> decimal_scale;

  Field(std::string name, DataType type,
        Repetition rep = Repetition::kRequired)
      : name(std::move(name)), data_type(std::move(type)), repetition(rep) {}

  bool IsNullable() const { return repetition != Repetition::kRequired; }

  std::string ToString() const {
    std::string rep_str;
    switch (repetition) {
      case Repetition::kRequired:
        rep_str = "required";
        break;
      case Repetition::kOptional:
        rep_str = "optional";
        break;
      case Repetition::kRepeated:
        rep_str = "repeated";
        break;
    }
    return std::format("Field(name={}, dataType={}, repetition={})", name,
                       GetTypeName(data_type), rep_str);
  }
};

// Schema definition (similar to Arrow Schema)
class Schema {
 public:
  Schema() = default;
  explicit Schema(std::vector<Field> fields) : fields_(std::move(fields)) {}

  const std::vector<Field>& fields() const { return fields_; }
  size_t num_fields() const { return fields_.size(); }

  const Field& field(size_t index) const {
    assert(index < fields_.size() && "field index out of range");
    return fields_[index];
  }

  std::optional<size_t> GetFieldIndex(const std::string& name) const {
    auto it = std::ranges::find_if(
        fields_, [&name](const Field& f) { return f.name == name; });
    if (it != fields_.end()) {
      return static_cast<size_t>(std::distance(fields_.begin(), it));
    }
    return std::nullopt;
  }

  const Field* GetFieldByName(const std::string& name) const {
    auto idx = GetFieldIndex(name);
    return idx.has_value() ? &fields_[*idx] : nullptr;
  }

  [[nodiscard]] Result<Schema> Project(const std::vector<size_t>& indices) const {
    std::vector<Field> projected_fields;
    projected_fields.reserve(indices.size());
    for (size_t idx : indices) {
      if (idx >= fields_.size()) {
        return Error::OutOfRange(std::format("Projection index {} out of range", idx));
      }
      projected_fields.push_back(fields_[idx]);
    }
    return Schema(std::move(projected_fields));
  }

  [[nodiscard]] Result<Schema> Select(const std::vector<std::string>& names) const {
    std::vector<Field> selected_fields;
    selected_fields.reserve(names.size());
    for (const auto& name : names) {
      auto idx = GetFieldIndex(name);
      if (!idx.has_value()) {
        return Error::SchemaError(std::format("Field '{}' not found in schema", name));
      }
      selected_fields.push_back(fields_[*idx]);
    }
    return Schema(std::move(selected_fields));
  }

  std::string ToString() const {
    std::string result = "Schema(fields=[";
    for (size_t i = 0; i < fields_.size(); ++i) {
      if (i > 0) result += ", ";
      result += fields_[i].ToString();
    }
    result += "])";
    return result;
  }

 private:
  std::vector<Field> fields_;
};

}  // namespace quest::columnar
