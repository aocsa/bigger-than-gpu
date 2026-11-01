// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/operators/hash_join_operator.hpp"

#include <format>
#include <utility>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"

namespace quest::taskflow {

namespace {

/// @brief Looks up a join key column and checks it is Int32 or Int64.
[[nodiscard]] Result<const columnar::ColumnVector*> KeyColumn(const columnar::RecordBatch& batch,
                                                              const std::string& name,
                                                              std::string_view side) {
  const auto* column = batch.GetColumnByName(name);
  if (column == nullptr) {
    return Error::SchemaError(
        std::format("HashJoinOperator: {} key column '{}' not found", side, name));
  }
  const auto type = column->GetType();
  if (type != columnar::DataType::kInt32 && type != columnar::DataType::kInt64) {
    return Error::TypeError(
        std::format("HashJoinOperator: {} key column '{}' is {}, not an integer", side, name,
                    columnar::GetTypeName(type)));
  }
  return column;
}

/// @brief Calls @p f(row, key) for each non-null key of @p column (checked by KeyColumn),
/// widened to int64.
template <typename F>
void ForEachKey(const columnar::ColumnVector& column, F&& f) {
  auto run = [&]<typename T>() {
    const auto keys = column.View<T>();
    for (std::size_t row = 0; row < keys.size(); ++row) {
      if (!keys.IsNull(row)) {
        f(row, static_cast<std::int64_t>(keys[row]));
      }
    }
  };
  if (column.GetType() == columnar::DataType::kInt32) {
    run.template operator()<std::int32_t>();
  } else {
    run.template operator()<std::int64_t>();
  }
}

}  // namespace

HashJoinOperator::HashJoinOperator(std::uint32_t operator_id, std::string build_key,
                                   std::string probe_key,
                                   std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : Operator(operator_id, "HashJoin", OperatorType::HashJoin),
      build_key_(std::move(build_key)),
      probe_key_(std::move(probe_key)),
      memory_manager_(std::move(memory_manager)) {}

PortRole HashJoinOperator::InputPortRole(std::string_view port) const {
  return port == "build" ? PortRole::kSink : PortRole::kFused;
}

Status HashJoinOperator::Sink(cache::DataBlockPtr batch) {
  auto host_result = cache::EnsureHostBlock(std::move(batch));
  if (!host_result) {
    return std::unexpected(host_result.error());
  }
  std::lock_guard lock(sink_mutex_);
  build_blocks_.push_back(std::move(*host_result));
  return {};
}

Status HashJoinOperator::FinalizeSink() {
  std::lock_guard lock(sink_mutex_);
  if (!build_blocks_.empty()) {
    auto merged = cache::ConcatenateHostBlocks(std::exchange(build_blocks_, {}))
                      .and_then(cache::EnsureHostBlock);
    if (!merged) {
      return std::unexpected(merged.error());
    }
    build_batch_.emplace(*(*merged)->record_batch());

    auto key_column = KeyColumn(*build_batch_, build_key_, "build");
    if (!key_column) {
      return std::unexpected(key_column.error());
    }
    ForEachKey(**key_column, [this](std::size_t row, std::int64_t key) {
      build_rows_by_key_[key].push_back(row);
    });
  }
  finalized_ = true;
  return {};
}

Result<cache::DataBlockPtr> HashJoinOperator::ProcessBatch(cache::DataBlockPtr input) {
  if (!finalized_) {
    return Error::Invalid("HashJoinOperator: probe batch arrived before FinalizeSink()");
  }
  auto host_result = cache::EnsureHostBlock(std::move(input));
  if (!host_result) {
    return std::unexpected(host_result.error());
  }
  auto host = std::move(*host_result);
  const auto* probe = host->record_batch();
  auto key_column = KeyColumn(*probe, probe_key_, "probe");
  if (!key_column) {
    return std::unexpected(key_column.error());
  }

  std::vector<std::size_t> probe_rows;
  std::vector<std::size_t> build_rows;
  ForEachKey(**key_column, [&](std::size_t row, std::int64_t key) {
    auto match = build_rows_by_key_.find(key);
    if (match == build_rows_by_key_.end()) {
      return;
    }
    for (std::size_t build_row : match->second) {
      probe_rows.push_back(row);
      build_rows.push_back(build_row);
    }
  });

  std::vector<columnar::Field> fields = probe->schema().fields();
  auto columns = execution::GatherColumns(*probe, probe_rows);
  if (build_batch_) {
    fields.append_range(build_batch_->schema().fields());
    columns.append_range(execution::GatherColumns(*build_batch_, build_rows));
  }

  return cache::MakeTrackedHostBlock(
      std::format("hash_join_{}", id()),
      columnar::RecordBatch(columnar::Schema(std::move(fields)), std::move(columns)),
      memory_manager_);
}

}  // namespace quest::taskflow
