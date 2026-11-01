// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/operators/sort_operator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <format>
#include <functional>
#include <numeric>
#include <utility>

#include "quest/columnar/record_batch.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"

namespace quest::taskflow {

namespace {

/// @brief Orders two rows by one key: below, equal to or above zero.
using RowOrder = std::function<int(std::size_t, std::size_t)>;

/// @brief The order of @p column's rows: NULLS LAST in both directions, and NaN above every
/// other number (NaN compares false with everything, which would break the strict weak
/// ordering stable_sort requires; DuckDB orders NaN this way).
[[nodiscard]] RowOrder MakeRowOrder(const columnar::ColumnVector& column, bool ascending) {
  return columnar::VisitType(column.GetType(), [&]<typename Tag>(Tag) -> RowOrder {
    using T = typename Tag::CType;
    return [view = column.View<T>(), ascending](std::size_t a, std::size_t b) {
      const bool a_null = view.IsNull(a);
      const bool b_null = view.IsNull(b);
      if (a_null || b_null) {
        return a_null == b_null ? 0 : (a_null ? 1 : -1);
      }
      const T x = view[a];
      const T y = view[b];
      int order = x < y ? -1 : (y < x ? 1 : 0);
      if constexpr (std::is_floating_point_v<T>) {
        if (std::isnan(x) || std::isnan(y)) {
          order = std::isnan(x) == std::isnan(y) ? 0 : (std::isnan(x) ? 1 : -1);
        }
      }
      return ascending ? order : -order;
    };
  });
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

  auto host = cache::ConcatenateHostBlocks(std::exchange(batches_, {}))
                  .and_then(cache::EnsureHostBlock);
  if (!host) {
    return std::unexpected(host.error());
  }
  const columnar::RecordBatch& merged = *(*host)->record_batch();
  const std::size_t num_rows = merged.num_rows();

  std::vector<RowOrder> orders;
  orders.reserve(keys_.size());
  for (const auto& key : keys_) {
    auto index = merged.schema().GetFieldIndex(key.column);
    if (!index) {
      return Error::SchemaError(std::format("SortOperator: column '{}' not found", key.column));
    }
    orders.push_back(MakeRowOrder(merged.column(*index), key.ascending));
  }

  std::vector<std::size_t> order(num_rows);
  std::iota(order.begin(), order.end(), std::size_t{0});
  std::ranges::stable_sort(order, [&](std::size_t a, std::size_t b) {
    for (const auto& by_key : orders) {
      if (const int c = by_key(a, b); c != 0) {
        return c < 0;
      }
    }
    return false;
  });

  output_ = cache::MakeTrackedHostBlock(
      std::format("sort_{}", id()),
      columnar::RecordBatch(merged.schema(), execution::GatherColumns(merged, order)),
      memory_manager_);
  finalized_.store(true);
  return {};
}

Result<std::optional<cache::DataBlockPtr>> SortOperator::GetNextBatch() {
  std::lock_guard lock(mutex_);
  if (!finalized_.load()) {
    return Error::Invalid("SortOperator: GetNextBatch before FinalizeSink");
  }
  return std::exchange(output_, std::nullopt);
}

}  // namespace quest::taskflow
