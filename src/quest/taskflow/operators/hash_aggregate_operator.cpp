// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/operators/hash_aggregate_operator.hpp"

#include <cstddef>
#include <format>
#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

namespace quest::taskflow {

namespace {

using columnar::ColumnVector;
using columnar::ColumnVectorPtr;
using columnar::DataType;
using columnar::Value;

/// @brief How SUM adds an input: integers and Decimals (unscaled) in an int64, the rest as double.
enum class NumericClass : std::uint8_t { kInteger, kDecimal, kFloating, kOther };

[[nodiscard]] NumericClass Classify(DataType type) {
  if (columnar::IsInteger(type)) {
    return NumericClass::kInteger;
  }
  if (columnar::IsDecimal(type)) {
    return NumericClass::kDecimal;
  }
  return columnar::IsNumeric(type) ? NumericClass::kFloating : NumericClass::kOther;
}

/// @brief Numeric value of @p value as @p T; only called on values of a column that
///        Classify() admitted as numeric.
template <typename T>
[[nodiscard]] T NumericAs(const Value& value) {
  return std::visit(
      [](const auto& v) -> T {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, bool>) {
          return static_cast<T>(v ? 1 : 0);
        } else if constexpr (std::is_same_v<V, columnar::DecimalValue>) {
          // SUM adds the unscaled digits; AVG adds the value.
          if constexpr (std::is_integral_v<T>) {
            return static_cast<T>(v.unscaled);
          } else {
            return static_cast<T>(columnar::ToDoubleValue(v));
          }
        } else if constexpr (std::is_arithmetic_v<V>) {
          return static_cast<T>(v);
        } else {
          std::unreachable();
        }
      },
      value);
}

[[nodiscard]] std::size_t HashValue(const Value& value) {
  return std::visit(
      [](const auto& v) -> std::size_t {
        using V = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<V, std::monostate>) {
          return 0;
        } else if constexpr (std::is_same_v<V, columnar::Date32>) {
          return std::hash<std::int32_t>{}(v.days);
        } else if constexpr (std::is_same_v<V, columnar::Timestamp>) {
          return std::hash<std::int64_t>{}(v.micros);
        } else if constexpr (std::is_same_v<V, columnar::DecimalValue>) {
          return std::hash<std::int64_t>{}(v.unscaled);  // one column, one scale
        } else {
          return std::hash<V>{}(v);
        }
      },
      value);
}

[[nodiscard]] bool IsNullValue(const Value& value) {
  return std::holds_alternative<std::monostate>(value);
}

[[nodiscard]] DataType OutputType(AggregateKind kind, const std::optional<DataType>& input_type) {
  switch (kind) {
    case AggregateKind::kCount:
    case AggregateKind::kCountStar:
      return columnar::DataType::kInt64;
    case AggregateKind::kAvg:
      return columnar::DataType::kDouble;
    case AggregateKind::kSum:
      if (input_type && Classify(*input_type) == NumericClass::kFloating) {
        return columnar::DataType::kDouble;
      }
      if (input_type && columnar::IsDecimal(*input_type)) {
        // DuckDB widens to DECIMAL(38, s); QUEST keeps an int64 and fails on overflow.
        return columnar::DataType::Decimal(columnar::DataType::kMaxDecimalPrecision,
                                           input_type->scale);
      }
      return columnar::DataType::kInt64;
    case AggregateKind::kMin:
    case AggregateKind::kMax:
      return input_type.value_or(columnar::DataType::kInt64);
  }
  std::unreachable();
}

[[nodiscard]] Status CheckedAdd(std::int64_t& sum, std::int64_t addend, std::string_view name) {
  if (__builtin_add_overflow(sum, addend, &sum)) {
    return Error::OutOfRange(std::format("HashAggregateOperator: SUM overflow in '{}'", name));
  }
  return {};
}

}  // namespace

HashAggregateOperator::HashAggregateOperator(
    std::uint32_t operator_id, std::vector<std::string> group_by,
    std::vector<AggregateSpec> aggregates,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : Operator(operator_id, "HashAggregate", OperatorType::HashAggregate),
      group_by_(std::move(group_by)),
      aggregates_(std::move(aggregates)),
      memory_manager_(std::move(memory_manager)),
      input_types_(aggregates_.size()) {}

std::size_t HashAggregateOperator::KeyHash::operator()(
    const std::vector<columnar::Value>& key) const noexcept {
  std::size_t seed = key.size();
  for (const auto& value : key) {
    seed ^= HashValue(value) + 0x9e3779b97f4a7c15ULL + (seed << 6) + (seed >> 2);
  }
  return seed;
}

Status HashAggregateOperator::Sink(cache::DataBlockPtr batch) {
  if (finalized_.load()) {
    return Error::Invalid("HashAggregateOperator: Sink after FinalizeSink");
  }
  if (!batch || batch->num_rows() == 0) {
    return {};
  }
  auto host = cache::EnsureHostBlock(std::move(batch));
  if (!host) {
    return std::unexpected(host.error());
  }
  const columnar::RecordBatch* input = (*host)->record_batch();
  if (input == nullptr) {
    return Error::Invalid("HashAggregateOperator: empty record batch");
  }

  std::vector<const ColumnVector*> key_columns;
  std::vector<columnar::Field> key_fields;
  for (const auto& name : group_by_) {
    auto index = input->schema().GetFieldIndex(name);
    if (!index) {
      return Error::SchemaError(
          std::format("HashAggregateOperator: group-by column '{}' not found", name));
    }
    key_columns.push_back(&input->column(*index));
    key_fields.push_back(input->schema().field(*index));
  }

  const std::size_t num_aggregates = aggregates_.size();
  std::vector<ColumnVectorPtr> inputs(num_aggregates);
  std::vector<NumericClass> classes(num_aggregates, NumericClass::kOther);
  for (std::size_t i = 0; i < num_aggregates; ++i) {
    const auto& spec = aggregates_[i];
    if (spec.kind == AggregateKind::kCountStar) {
      continue;
    }
    auto evaluated = spec.input->Evaluate(*input);
    if (!evaluated) {
      return std::unexpected(evaluated.error());
    }
    inputs[i] = std::move(*evaluated);
    classes[i] = Classify(inputs[i]->GetType());
    // DuckDB: SUM(BOOLEAN) counts the true values; AVG(BOOLEAN) is an error.
    if (spec.kind == AggregateKind::kSum && inputs[i]->GetType() == DataType::kBool) {
      classes[i] = NumericClass::kInteger;
    }
    if ((spec.kind == AggregateKind::kSum || spec.kind == AggregateKind::kAvg) &&
        classes[i] == NumericClass::kOther) {
      return Error::TypeError(
          std::format("HashAggregateOperator: '{}' needs a numeric input, got {}", spec.output_name,
                      columnar::GetTypeName(inputs[i]->GetType())));
    }
  }

  GroupTable local;
  std::vector<Value> key;
  for (std::size_t row = 0; row < input->num_rows(); ++row) {
    key.clear();
    for (const auto* column : key_columns) {
      key.push_back(column->GetValue(row));
    }
    auto [entry, inserted] = local.index.try_emplace(key, local.groups.size());
    if (inserted) {
      local.groups.push_back(Group{key, std::vector<AggregateState>(num_aggregates)});
    }
    auto& states = local.groups[entry->second].states;

    for (std::size_t i = 0; i < num_aggregates; ++i) {
      const auto kind = aggregates_[i].kind;
      auto& state = states[i];
      if (kind == AggregateKind::kCountStar) {
        ++state.count;
        continue;
      }
      if (inputs[i]->IsNull(row)) {
        continue;
      }
      ++state.count;
      switch (kind) {
        case AggregateKind::kSum:
          if (classes[i] == NumericClass::kInteger || classes[i] == NumericClass::kDecimal) {
            auto added =
                CheckedAdd(state.int_sum, NumericAs<std::int64_t>(inputs[i]->GetValue(row)),
                           aggregates_[i].output_name);
            if (!added) {
              return added;
            }
          } else {
            state.double_sum += NumericAs<double>(inputs[i]->GetValue(row));
          }
          break;
        case AggregateKind::kAvg:
          state.double_sum += NumericAs<double>(inputs[i]->GetValue(row));
          break;
        case AggregateKind::kMin:
        case AggregateKind::kMax: {
          Value value = inputs[i]->GetValue(row);
          const bool better =
              kind == AggregateKind::kMin ? value < state.extreme : state.extreme < value;
          if (IsNullValue(state.extreme) || better) {
            state.extreme = std::move(value);
          }
          break;
        }
        case AggregateKind::kCount:
        case AggregateKind::kCountStar:
          break;
      }
    }
  }

  std::lock_guard lock(mutex_);
  if (!group_fields_) {
    group_fields_ = std::move(key_fields);
  }
  for (std::size_t i = 0; i < num_aggregates; ++i) {
    if (!input_types_[i] && inputs[i]) {
      input_types_[i] = inputs[i]->GetType();
    }
  }
  for (auto& group : local.groups) {
    auto [entry, inserted] = table_.index.try_emplace(group.key, table_.groups.size());
    if (inserted) {
      table_.groups.push_back(std::move(group));
      continue;
    }
    auto& states = table_.groups[entry->second].states;
    for (std::size_t i = 0; i < num_aggregates; ++i) {
      auto& dest = states[i];
      auto& src = group.states[i];
      dest.count += src.count;
      dest.double_sum += src.double_sum;
      if (auto added = CheckedAdd(dest.int_sum, src.int_sum, aggregates_[i].output_name); !added) {
        return added;
      }
      if (IsNullValue(src.extreme)) {
        continue;
      }
      const bool better = aggregates_[i].kind == AggregateKind::kMin ? src.extreme < dest.extreme
                                                                     : dest.extreme < src.extreme;
      if (IsNullValue(dest.extreme) || better) {
        dest.extreme = std::move(src.extreme);
      }
    }
  }
  return {};
}

Status HashAggregateOperator::FinalizeSink() {
  std::lock_guard lock(mutex_);
  if (finalized_.load()) {
    return Error::Invalid("HashAggregateOperator: FinalizeSink called twice");
  }
  if (group_by_.empty() && table_.groups.empty()) {
    table_.groups.push_back(Group{{}, std::vector<AggregateState>(aggregates_.size())});
  }
  if (!table_.groups.empty()) {
    auto output = BuildOutput();
    if (!output) {
      return std::unexpected(output.error());
    }
    output_ = cache::MakeTrackedHostBlock(std::format("hash_aggregate_{}", id()),
                                          std::move(*output), memory_manager_);
  }
  finalized_.store(true);
  return {};
}

Result<std::optional<cache::DataBlockPtr>> HashAggregateOperator::GetNextBatch() {
  std::lock_guard lock(mutex_);
  if (!finalized_.load()) {
    return Error::Invalid("HashAggregateOperator: GetNextBatch before FinalizeSink");
  }
  std::optional<cache::DataBlockPtr> next = std::move(output_);
  output_.reset();
  return next;
}

Result<columnar::RecordBatch> HashAggregateOperator::BuildOutput() const {
  std::vector<columnar::Field> fields;
  std::vector<ColumnVectorPtr> columns;
  std::vector<Value> values;
  values.reserve(table_.groups.size());

  for (std::size_t k = 0; k < group_by_.size(); ++k) {
    const auto& field = (*group_fields_)[k];
    values.clear();
    for (const auto& group : table_.groups) {
      values.push_back(group.key[k]);
    }
    auto column = columnar::ColumnFromValues(field.data_type, values);
    if (!column) {
      return std::unexpected(column.error());
    }
    fields.push_back(field);
    columns.push_back(std::move(*column));
  }

  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const auto& spec = aggregates_[i];
    const DataType type = input_types_[i] || !spec.output_type
                              ? OutputType(spec.kind, input_types_[i])
                              : *spec.output_type;
    const bool floating_sum = Classify(type) == NumericClass::kFloating;
    values.clear();
    for (const auto& group : table_.groups) {
      const auto& state = group.states[i];
      switch (spec.kind) {
        case AggregateKind::kCount:
        case AggregateKind::kCountStar:
          values.emplace_back(state.count);
          break;
        case AggregateKind::kSum:
          if (state.count == 0) {
            values.emplace_back(std::monostate{});
          } else if (floating_sum) {
            values.emplace_back(state.double_sum);
          } else if (columnar::IsDecimal(type)) {
            values.emplace_back(columnar::DecimalValue{state.int_sum, type.scale});
          } else {
            values.emplace_back(state.int_sum);
          }
          break;
        case AggregateKind::kAvg:
          if (state.count == 0) {
            values.emplace_back(std::monostate{});
          } else {
            values.emplace_back(state.double_sum / static_cast<double>(state.count));
          }
          break;
        case AggregateKind::kMin:
        case AggregateKind::kMax:
          values.push_back(state.extreme);
          break;
      }
    }
    auto column = columnar::ColumnFromValues(type, values);
    if (!column) {
      return std::unexpected(column.error());
    }
    fields.emplace_back(spec.output_name, type, columnar::Repetition::kOptional);
    columns.push_back(std::move(*column));
  }

  return columnar::RecordBatch(columnar::Schema(std::move(fields)), std::move(columns));
}

}  // namespace quest::taskflow
