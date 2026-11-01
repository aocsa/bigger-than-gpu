// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/operators/hash_aggregate_operator.hpp"

#include <cmath>
#include <cstddef>
#include <cstring>
#include <format>
#include <functional>
#include <limits>
#include <string_view>
#include <type_traits>
#include <utility>

namespace quest::taskflow {

namespace {

using columnar::ColumnBuilder;
using columnar::ColumnVector;
using columnar::ColumnVectorPtr;
using columnar::DataType;
using columnar::Decimal64;

/// @brief How SUM adds an input: integers, Bools and Decimals (unscaled) in an int64, the
/// rest as double.
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

// =============================================================================
// Group keys
// =============================================================================

/// @brief Appends row r of a key column to an encoded key.
using KeyEncoder = std::function<void(std::string&, std::size_t)>;

/// @brief Encodes @p column's cells: a validity byte, then the value bytes; a String has a
/// length prefix, and a float is canonical (-0.0 as 0.0, one NaN) so equal keys are equal
/// bytes.
[[nodiscard]] KeyEncoder MakeKeyEncoder(const ColumnVector& column) {
  return columnar::VisitType(column.GetType(), [&]<typename Tag>(Tag) -> KeyEncoder {
    using T = typename Tag::CType;
    return [view = column.View<T>()](std::string& key, std::size_t row) {
      if (view.IsNull(row)) {
        key.push_back('\0');
        return;
      }
      key.push_back('\1');
      T value = view[row];
      if constexpr (std::same_as<T, std::string_view>) {
        const auto length = static_cast<std::uint64_t>(value.size());
        key.append(reinterpret_cast<const char*>(&length), sizeof(length));
        key.append(value);
        return;
      } else {
        if constexpr (std::is_floating_point_v<T>) {
          if (value == T{0}) {
            value = T{0};
          } else if (std::isnan(value)) {
            value = std::numeric_limits<T>::quiet_NaN();
          }
        }
        const auto stored = static_cast<columnar::StorageOf<T>>(value);
        key.append(reinterpret_cast<const char*>(&stored), sizeof(stored));
      }
    };
  });
}

/// @brief Decodes one key cell at @p pos (as MakeKeyEncoder wrote it) into @p builder.
template <typename T>
void DecodeKeyCell(std::string_view key, std::size_t& pos, ColumnBuilder<T>& builder) {
  if (key[pos++] == '\0') {
    builder.AppendNull();
    return;
  }
  if constexpr (std::same_as<T, std::string_view>) {
    std::uint64_t length = 0;
    std::memcpy(&length, key.data() + pos, sizeof(length));
    pos += sizeof(length);
    builder.Append(key.substr(pos, length));
    pos += length;
  } else {
    columnar::StorageOf<T> stored{};
    std::memcpy(&stored, key.data() + pos, sizeof(stored));
    pos += sizeof(stored);
    builder.Append(static_cast<T>(stored));
  }
}

// =============================================================================
// Input values
// =============================================================================

/// @brief A SUM addend: a Bool as 0 or 1, a Decimal unscaled, an integer as is.
template <typename T>
[[nodiscard]] std::int64_t IntegerAddend(T value) {
  if constexpr (std::same_as<T, Decimal64>) {
    return value.unscaled;
  } else {
    return static_cast<std::int64_t>(value);
  }
}

/// @brief A value for AVG or a floating SUM; a Decimal divides by 10^scale.
template <typename T>
[[nodiscard]] double AsDouble(T value, int scale) {
  if constexpr (std::same_as<T, Decimal64>) {
    return static_cast<double>(value.unscaled) /
           static_cast<double>(columnar::PowerOfTen(scale));
  } else {
    return static_cast<double>(value);
  }
}

}  // namespace

// =============================================================================
// MIN / MAX states
// =============================================================================

namespace {

/// @brief MIN (@p kMin) or MAX of C++ type @p T for every group; a String is kept owned,
/// since the batch it came from goes away.
template <typename T, bool kMin>
class TypedExtremes final : public HashAggregateOperator::ExtremesBase {
 public:
  void Update(const ColumnVector& input, std::span<const std::uint32_t> group_of_row) override {
    const auto view = input.View<T>();
    for (std::size_t row = 0; row < group_of_row.size(); ++row) {
      if (!view.IsNull(row)) {
        Fold(group_of_row[row], view[row]);
      }
    }
  }

  void Merge(const ExtremesBase& other, std::span<const std::uint32_t> remap) override {
    const auto& typed = static_cast<const TypedExtremes&>(other);
    for (std::size_t g = 0; g < typed.values_.size(); ++g) {
      if (typed.values_[g]) {
        Fold(remap[g], *typed.values_[g]);
      }
    }
  }

  [[nodiscard]] ColumnVectorPtr Finish(DataType type, std::size_t groups) const override {
    ColumnBuilder<T> builder(type, groups);
    for (std::size_t g = 0; g < groups; ++g) {
      g < values_.size() && values_[g] ? builder.Append(T(*values_[g])) : builder.AppendNull();
    }
    return std::move(builder).Finish();
  }

 private:
  using Stored = std::conditional_t<std::same_as<T, std::string_view>, std::string, T>;

  template <typename V>
  void Fold(std::uint32_t group, const V& value) {
    if (group >= values_.size()) {
      values_.resize(group + 1);
    }
    auto& current = values_[group];
    if (!current || (kMin ? value < *current : *current < value)) {
      current = Stored(value);
    }
  }

  std::vector<std::optional<Stored>> values_;
};

}  // namespace

HashAggregateOperator::HashAggregateOperator(
    std::uint32_t operator_id, std::vector<std::string> group_by,
    std::vector<AggregateSpec> aggregates,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : Operator(operator_id, "HashAggregate", OperatorType::HashAggregate),
      group_by_(std::move(group_by)),
      aggregates_(std::move(aggregates)),
      memory_manager_(std::move(memory_manager)),
      input_types_(aggregates_.size()) {
  table_.states.resize(aggregates_.size());
}

std::uint32_t HashAggregateOperator::GroupTable::FindOrInsert(const std::string& key) {
  const auto [entry, inserted] = index.try_emplace(key, static_cast<std::uint32_t>(keys.size()));
  if (inserted) {
    keys.push_back(key);
    for (auto& state : states) {
      state.count.push_back(0);
      state.int_sum.push_back(0);
      state.double_sum.push_back(0.0);
    }
  }
  return entry->second;
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

  std::vector<KeyEncoder> encoders;
  std::vector<columnar::Field> key_fields;
  for (const auto& name : group_by_) {
    auto index = input->schema().GetFieldIndex(name);
    if (!index) {
      return Error::SchemaError(
          std::format("HashAggregateOperator: group-by column '{}' not found", name));
    }
    encoders.push_back(MakeKeyEncoder(input->column(*index)));
    key_fields.push_back(input->schema().field(*index));
  }

  const std::size_t num_aggregates = aggregates_.size();
  std::vector<ColumnVectorPtr> inputs(num_aggregates);
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
    // DuckDB: SUM(BOOLEAN) counts the true values; AVG(BOOLEAN) is an error.
    const DataType type = inputs[i]->GetType();
    const bool summable = Classify(type) != NumericClass::kOther ||
                          (spec.kind == AggregateKind::kSum && type == DataType::kBool);
    if ((spec.kind == AggregateKind::kSum || spec.kind == AggregateKind::kAvg) && !summable) {
      return Error::TypeError(std::format("HashAggregateOperator: '{}' needs a numeric input, got {}",
                                          spec.output_name, columnar::GetTypeName(type)));
    }
  }

  // Group id of every row in a batch-local table.
  GroupTable local;
  local.states.resize(num_aggregates);
  const std::size_t rows = input->num_rows();
  std::vector<std::uint32_t> group_of_row(rows);
  std::string key;
  for (std::size_t row = 0; row < rows; ++row) {
    key.clear();
    for (const auto& encode : encoders) {
      encode(key, row);
    }
    group_of_row[row] = local.FindOrInsert(key);
  }

  // One typed pass over each aggregate's input.
  for (std::size_t i = 0; i < num_aggregates; ++i) {
    const auto& spec = aggregates_[i];
    auto& state = local.states[i];
    if (spec.kind == AggregateKind::kCountStar) {
      for (const auto group : group_of_row) {
        ++state.count[group];
      }
      continue;
    }
    const ColumnVector& column = *inputs[i];
    const DataType type = column.GetType();
    if (spec.kind == AggregateKind::kMin || spec.kind == AggregateKind::kMax) {
      if (!state.extremes) {
        state.extremes = MakeExtremes(spec.kind, type);
      }
      state.extremes->Update(column, group_of_row);
    }
    auto status = columnar::VisitType(type, [&]<typename Tag>(Tag) -> Status {
      using T = typename Tag::CType;
      const auto view = column.View<T>();
      for (std::size_t row = 0; row < rows; ++row) {
        if (view.IsNull(row)) {
          continue;
        }
        const auto group = group_of_row[row];
        ++state.count[group];
        if constexpr (std::is_arithmetic_v<T> || std::same_as<T, Decimal64>) {
          const bool integer_sum = std::is_integral_v<T> || std::same_as<T, Decimal64>;
          if (spec.kind == AggregateKind::kSum && integer_sum) {
            if (auto added = CheckedAdd(state.int_sum[group], IntegerAddend(view[row]),
                                        spec.output_name);
                !added) {
              return added;
            }
          } else if (spec.kind == AggregateKind::kSum || spec.kind == AggregateKind::kAvg) {
            state.double_sum[group] += AsDouble(view[row], type.scale);
          }
        }
      }
      return {};
    });
    if (!status) {
      return status;
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
  return MergeLocked(local);
}

std::unique_ptr<HashAggregateOperator::ExtremesBase> HashAggregateOperator::MakeExtremes(
    AggregateKind kind, DataType type) {
  return columnar::VisitType(type, [&]<typename Tag>(Tag) -> std::unique_ptr<ExtremesBase> {
    using T = typename Tag::CType;
    if (kind == AggregateKind::kMin) {
      return std::make_unique<TypedExtremes<T, true>>();
    }
    return std::make_unique<TypedExtremes<T, false>>();
  });
}

Status HashAggregateOperator::MergeLocked(GroupTable& local) {
  std::vector<std::uint32_t> remap(local.keys.size());
  for (std::size_t g = 0; g < local.keys.size(); ++g) {
    remap[g] = table_.FindOrInsert(local.keys[g]);
  }
  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    auto& dest = table_.states[i];
    const auto& src = local.states[i];
    for (std::size_t g = 0; g < remap.size(); ++g) {
      const auto target = remap[g];
      dest.count[target] += src.count[g];
      dest.double_sum[target] += src.double_sum[g];
      if (auto added = CheckedAdd(dest.int_sum[target], src.int_sum[g], aggregates_[i].output_name);
          !added) {
        return added;
      }
    }
    if (src.extremes) {
      if (!dest.extremes) {
        dest.extremes = MakeExtremes(aggregates_[i].kind, *input_types_[i]);
      }
      dest.extremes->Merge(*src.extremes, remap);
    }
  }
  return {};
}

Status HashAggregateOperator::FinalizeSink() {
  std::lock_guard lock(mutex_);
  if (finalized_.load()) {
    return Error::Invalid("HashAggregateOperator: FinalizeSink called twice");
  }
  if (group_by_.empty() && table_.keys.empty()) {
    table_.FindOrInsert(std::string{});  // one global group, even for empty input
  }
  if (!table_.keys.empty()) {
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
  const std::size_t groups = table_.keys.size();
  std::vector<columnar::Field> fields;
  std::vector<ColumnVectorPtr> columns;

  // Decode the key columns from the encoded keys, column by column.
  std::vector<std::size_t> positions(groups, 0);
  for (std::size_t k = 0; k < group_by_.size(); ++k) {
    const auto& field = (*group_fields_)[k];
    columns.push_back(columnar::VisitType(field.data_type, [&]<typename Tag>(Tag) {
      ColumnBuilder<typename Tag::CType> builder(field.data_type, groups);
      for (std::size_t g = 0; g < groups; ++g) {
        DecodeKeyCell(table_.keys[g], positions[g], builder);
      }
      return std::move(builder).Finish();
    }));
    fields.push_back(field);
  }

  for (std::size_t i = 0; i < aggregates_.size(); ++i) {
    const auto& spec = aggregates_[i];
    const auto& state = table_.states[i];
    const DataType type = input_types_[i] || !spec.output_type
                              ? OutputType(spec.kind, input_types_[i])
                              : *spec.output_type;
    ColumnVectorPtr column;
    switch (spec.kind) {
      case AggregateKind::kCount:
      case AggregateKind::kCountStar: {
        ColumnBuilder<std::int64_t> builder(type, groups);
        for (std::size_t g = 0; g < groups; ++g) {
          builder.Append(state.count[g]);
        }
        column = std::move(builder).Finish();
        break;
      }
      case AggregateKind::kSum:
        column = columnar::VisitType(type, [&]<typename Tag>(Tag) -> ColumnVectorPtr {
          using T = typename Tag::CType;
          ColumnBuilder<T> builder(type, groups);
          for (std::size_t g = 0; g < groups; ++g) {
            if (state.count[g] == 0) {
              builder.AppendNull();
            } else if constexpr (std::same_as<T, Decimal64>) {
              builder.Append(Decimal64{state.int_sum[g]});
            } else if constexpr (std::is_floating_point_v<T>) {
              builder.Append(static_cast<T>(state.double_sum[g]));
            } else if constexpr (std::is_integral_v<T>) {
              builder.Append(static_cast<T>(state.int_sum[g]));
            } else {
              builder.AppendNull();  // OutputType gives SUM only numeric types
            }
          }
          return std::move(builder).Finish();
        });
        break;
      case AggregateKind::kAvg: {
        ColumnBuilder<double> builder(type, groups);
        for (std::size_t g = 0; g < groups; ++g) {
          state.count[g] == 0
              ? builder.AppendNull()
              : builder.Append(state.double_sum[g] / static_cast<double>(state.count[g]));
        }
        column = std::move(builder).Finish();
        break;
      }
      case AggregateKind::kMin:
      case AggregateKind::kMax:
        column = state.extremes ? state.extremes->Finish(type, groups)
                                : columnar::VisitType(type, [&]<typename Tag>(Tag) {
                                    ColumnBuilder<typename Tag::CType> builder(type, groups);
                                    for (std::size_t g = 0; g < groups; ++g) {
                                      builder.AppendNull();
                                    }
                                    return std::move(builder).Finish();
                                  });
        break;
    }
    fields.emplace_back(spec.output_name, type, columnar::Repetition::kOptional);
    columns.push_back(std::move(column));
  }

  return columnar::RecordBatch(columnar::Schema(std::move(fields)), std::move(columns));
}

}  // namespace quest::taskflow
