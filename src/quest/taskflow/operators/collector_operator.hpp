// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file collector_operator.hpp
/// @brief Terminal sink that accumulates final pipeline batches.

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Output sink for tests and ExecuteOnGraph collection.
class CollectorOperator : public Operator {
 public:
  explicit CollectorOperator(std::uint32_t operator_id)
      : Operator(operator_id, "Collector", OperatorType::Output) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  // --- Port contract: CollectorOperator always requires a Full barrier -------

  /// @brief DeclaredInputPorts is empty (undeclared): any port name is valid.
  ///
  /// The collector accepts data from any source, so port name checking is not
  /// enforced. The contract below applies to all inputs regardless of port.
  [[nodiscard]] std::vector<std::string_view> DeclaredInputPorts() const override { return {}; }

  /// @brief CollectorOperator requires Full barrier on every input port.
  ///
  /// This ensures producers flush completely before the collector drains,
  /// which is required for correct result accumulation. GraphBuilder::Build()
  /// infers this automatically when no explicit barrier is specified.
  [[nodiscard]] EdgeConfig InputEdgeContract(std::string_view /*port*/) const override {
    EdgeConfig cfg;
    cfg.barrier = BarrierType::Full;
    return cfg;
  }

  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override {
    if (!batch || batch->num_rows() == 0) {
      return {};
    }
    std::lock_guard lock(mutex_);
    total_rows_ += batch->num_rows();
    if (batch->tier() == memory::MemoryTier::Disk) {
      ++disk_tier_count_;
    }
    batches_.push_back(std::move(batch));
    return {};
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
  [[nodiscard]] Result<std::vector<columnar::RecordBatch>> CollectBatches() const {
    std::vector<columnar::RecordBatch> out;
    out.reserve(batches_.size());
    for (const auto& block : batches_) {
      auto host = cache::EnsureHostBlock(block);
      if (!host) {
        return std::unexpected(host.error());
      }
      out.push_back(*(*host)->record_batch());  // shares the columns
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
