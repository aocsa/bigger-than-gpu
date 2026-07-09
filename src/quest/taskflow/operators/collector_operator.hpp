// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_COLLECTOR_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_COLLECTOR_OPERATOR_HPP_

/// @file collector_operator.hpp
/// @brief Terminal sink that accumulates final pipeline batches.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Output sink for tests and ExecuteOnGraph collection.
class CollectorOperator : public Operator {
 public:
  explicit CollectorOperator(std::uint32_t operator_id)
      : Operator(operator_id, "Collector", OperatorType::Output) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  [[nodiscard]] ExecutionStatus run() override { return ExecutionStatus::Proceed; }

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override {
    if (!batch || batch->num_rows() == 0) {
      return Status::OK();
    }
    std::lock_guard lock(mutex_);
    total_rows_ += batch->num_rows();
    if (batch->tier() == memory::MemoryTier::Disk) {
      ++disk_tier_count_;
    }
    batches_.push_back(std::move(batch));
    return Status::OK();
  }

  // NOTE (thread-safety contract): Sink() locks because a future executor
  // may deliver batches from multiple tasks; the readers below deliberately
  // do NOT lock — they are only valid after Execute() has returned (single
  // drain per pipeline today, and Execute() joins all workers before
  // returning). Do not call them while the graph is running.
  [[nodiscard]] const std::vector<cache::DataBlockPtr>& batches() const noexcept {
    return batches_;
  }

  [[nodiscard]] std::size_t total_rows() const noexcept { return total_rows_; }

  [[nodiscard]] std::size_t disk_tier_count() const noexcept { return disk_tier_count_; }

  /// @brief Materialize collected blocks as RecordBatches (CPU upgrade if needed).
  [[nodiscard]] Result<std::vector<datasource::RecordBatch>> CollectBatches() const {
    std::vector<datasource::RecordBatch> out;
    out.reserve(batches_.size());
    for (const auto& block : batches_) {
      auto host = std::dynamic_pointer_cast<cache::HostMemoryBlock>(block);
      if (!host) {
        auto upgraded = block->ToCpu();
        if (!upgraded) {
          return std::unexpected(upgraded.error());
        }
        host = std::move(*upgraded);
      }
      if (!host->record_batch()) {
        return Status::Invalid("Collector: block has no record batch");
      }
      // Copy columns into a new RecordBatch (HostMemoryBlock owns the original).
      const auto& rb = *host->record_batch();
      std::vector<std::shared_ptr<datasource::ColumnVector>> cols;
      cols.reserve(rb.num_columns());
      for (size_t i = 0; i < rb.num_columns(); ++i) {
        cols.push_back(rb.column_ptr(i));
      }
      out.emplace_back(rb.schema(), std::move(cols));
    }
    return out;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<cache::DataBlockPtr> batches_;
  std::size_t total_rows_{0};
  std::size_t disk_tier_count_{0};
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_COLLECTOR_OPERATOR_HPP_
