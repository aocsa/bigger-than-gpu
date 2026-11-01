// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/operators/sort_operator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <numeric>
#include <utility>
#include <variant>

#include "quest/columnar/record_batch.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"

namespace quest::taskflow {

namespace {

[[nodiscard]] bool IsNaN(const columnar::Value& value) {
  if (const auto* d = std::get_if<double>(&value)) {
    return std::isnan(*d);
  }
  if (const auto* f = std::get_if<float>(&value)) {
    return std::isnan(*f);
  }
  return false;
}

}  // namespace

SortOperator::SortOperator(std::uint32_t operator_id, std::vector<SortKey> keys,
                           std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : Operator(operator_id, "Sort", OperatorType::Sort),
      keys_(std::move(keys)),
      memory_manager_(std::move(memory_manager)) {}

Status SortOperator::Sink(cache::DataBlockPtr batch) {
  if (finalized_.load()) {
    return Error::Invalid("SortOperator: Sink after FinalizeSink");
  }
  if (!batch || batch->num_rows() == 0) {
    return {};
  }
  std::lock_guard lock(mutex_);
  batches_.push_back(std::move(batch));
  return {};
}

Status SortOperator::FinalizeSink() {
  std::lock_guard lock(mutex_);
  if (finalized_.load()) {
    return Error::Invalid("SortOperator: FinalizeSink called twice");
  }
  if (batches_.empty()) {
    finalized_.store(true);
    return {};
  }

  std::vector<std::shared_ptr<cache::HostMemoryBlock>> hosts;
  std::vector<const columnar::RecordBatch*> inputs;
  for (auto& block : batches_) {
    auto host = cache::EnsureHostBlock(std::move(block));
    if (!host) {
      return std::unexpected(host.error());
    }
    if ((*host)->record_batch() == nullptr) {
      return Error::Invalid("SortOperator: empty record batch");
    }
    inputs.push_back((*host)->record_batch());
    hosts.push_back(std::move(*host));
  }
  batches_.clear();
  auto merged = columnar::ConcatenateRecordBatches(inputs);
  if (!merged) {
    return std::unexpected(merged.error());
  }
  const std::size_t num_rows = merged->num_rows();

  std::vector<std::vector<columnar::Value>> key_values;
  for (const auto& key : keys_) {
    auto index = merged->schema().GetFieldIndex(key.column);
    if (!index) {
      return Error::SchemaError(std::format("SortOperator: column '{}' not found", key.column));
    }
    const auto& column = merged->column(*index);
    auto& values = key_values.emplace_back();
    values.reserve(num_rows);
    for (std::size_t row = 0; row < num_rows; ++row) {
      values.push_back(column.GetValue(row));
    }
  }

  std::vector<std::size_t> order(num_rows);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
    for (std::size_t k = 0; k < keys_.size(); ++k) {
      const auto& x = key_values[k][a];
      const auto& y = key_values[k][b];
      const bool x_null = std::holds_alternative<std::monostate>(x);
      const bool y_null = std::holds_alternative<std::monostate>(y);
      if (x_null || y_null) {
        if (x_null && y_null) {
          continue;
        }
        return y_null;  // NULLS LAST in both directions
      }
      // NaN compares false with everything, which would break the strict weak ordering
      // stable_sort requires; DuckDB orders NaN above every other number.
      const bool x_nan = IsNaN(x);
      const bool y_nan = IsNaN(y);
      if (x_nan || y_nan) {
        if (x_nan && y_nan) {
          continue;
        }
        return keys_[k].ascending ? y_nan : x_nan;
      }
      if (x == y) {
        continue;
      }
      return keys_[k].ascending ? x < y : y < x;
    }
    return false;
  });

  std::vector<std::shared_ptr<columnar::ColumnVector>> columns;
  columns.reserve(merged->num_columns());
  for (std::size_t c = 0; c < merged->num_columns(); ++c) {
    columns.push_back(execution::GatherColumn(merged->column(c), order));
  }
  output_ = cache::MakeTrackedHostBlock(std::format("sort_{}", id()),
                                        columnar::RecordBatch(merged->schema(), std::move(columns)),
                                        memory_manager_);
  finalized_.store(true);
  return {};
}

Result<std::optional<cache::DataBlockPtr>> SortOperator::GetNextBatch() {
  std::lock_guard lock(mutex_);
  if (!finalized_.load()) {
    return Error::Invalid("SortOperator: GetNextBatch before FinalizeSink");
  }
  std::optional<cache::DataBlockPtr> next = std::move(output_);
  output_.reset();
  return next;
}

}  // namespace quest::taskflow
