// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/operators/hash_join_operator.hpp"

#include <format>
#include <utility>
#include <variant>

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
  if (!(type == columnar::DataType::kInt32) &&
      !(type == columnar::DataType::kInt64)) {
    return Error::TypeError(
        std::format("HashJoinOperator: {} key column '{}' is {}, not an integer", side, name,
                    columnar::GetTypeName(type)));
  }
  return column;
}

/// @brief The key at @p row widened to int64, or nullopt if it is null.
[[nodiscard]] std::optional<std::int64_t> KeyAt(const columnar::ColumnVector& column,
                                                std::size_t row) {
  const auto value = column.GetValue(row);
  if (const auto* key = std::get_if<std::int64_t>(&value)) {
    return *key;
  }
  if (const auto* key = std::get_if<std::int32_t>(&value)) {
    return *key;
  }
  return std::nullopt;
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
  if ((*host_result)->record_batch() == nullptr) {
    return Error::Invalid("HashJoinOperator: empty record batch");
  }
  std::lock_guard lock(sink_mutex_);
  build_blocks_.push_back(std::move(*host_result));
  return {};
}

Status HashJoinOperator::FinalizeSink() {
  std::lock_guard lock(sink_mutex_);
  if (!build_blocks_.empty()) {
    std::vector<const columnar::RecordBatch*> batches;
    batches.reserve(build_blocks_.size());
    for (const auto& block : build_blocks_) {
      batches.push_back(block->record_batch());
    }
    auto merged = columnar::ConcatenateRecordBatches(batches);
    if (!merged) {
      return std::unexpected(merged.error());
    }
    build_blocks_.clear();
    build_batch_.emplace(std::move(*merged));

    auto key_column = KeyColumn(*build_batch_, build_key_, "build");
    if (!key_column) {
      return std::unexpected(key_column.error());
    }
    for (std::size_t row = 0; row < build_batch_->num_rows(); ++row) {
      if (auto key = KeyAt(**key_column, row)) {
        build_rows_by_key_[*key].push_back(row);
      }
    }
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
  if (probe == nullptr) {
    return Error::Invalid("HashJoinOperator: empty record batch");
  }
  auto key_column = KeyColumn(*probe, probe_key_, "probe");
  if (!key_column) {
    return std::unexpected(key_column.error());
  }

  std::vector<std::size_t> probe_rows;
  std::vector<std::size_t> build_rows;
  for (std::size_t row = 0; row < probe->num_rows(); ++row) {
    auto key = KeyAt(**key_column, row);
    if (!key) {
      continue;
    }
    auto match = build_rows_by_key_.find(*key);
    if (match == build_rows_by_key_.end()) {
      continue;
    }
    for (std::size_t build_row : match->second) {
      probe_rows.push_back(row);
      build_rows.push_back(build_row);
    }
  }

  std::vector<columnar::Field> fields = probe->schema().fields();
  std::vector<std::shared_ptr<columnar::ColumnVector>> columns;
  columns.reserve(probe->num_columns() + (build_batch_ ? build_batch_->num_columns() : 0));
  for (std::size_t col = 0; col < probe->num_columns(); ++col) {
    columns.push_back(execution::GatherColumn(probe->column(col), probe_rows));
  }
  if (build_batch_) {
    const auto& build_fields = build_batch_->schema().fields();
    fields.insert(fields.end(), build_fields.begin(), build_fields.end());
    for (std::size_t col = 0; col < build_batch_->num_columns(); ++col) {
      columns.push_back(execution::GatherColumn(build_batch_->column(col), build_rows));
    }
  }

  return cache::MakeTrackedHostBlock(
      std::format("hash_join_{}", id()),
      columnar::RecordBatch(columnar::Schema(std::move(fields)), std::move(columns)),
      memory_manager_);
}

}  // namespace quest::taskflow
