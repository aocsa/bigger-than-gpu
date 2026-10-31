// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file limit_operator.hpp
/// @brief Streaming Limit that truncates the pipeline to the first N rows.
///
/// When remaining hits 0, ShouldStopSource() becomes true so the pipeline
/// drain loop stops claiming further source batches (single-pipeline early
/// stop). Cross-pipeline / multi-barrier cancellation is out of scope.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <utility>

#include "quest/cache/data_block.hpp"
#include "quest/execution/batch_kernels.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Streaming limit: claim rows from an atomic remaining counter.
///
/// After remaining hits 0, subsequent ProcessBatch calls return empty
/// HostMemoryBlocks, and ShouldStopSource() is true so PipelineExecutor
/// stops claiming further source batches in this pipeline.
class LimitOperator : public SimpleOperator {
 public:
  /// @param memory_manager Stamped onto output blocks so they stay visible
  ///        to saturation accounting and spillable at pipeline breaks.
  LimitOperator(std::uint32_t operator_id, std::size_t limit,
                std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr)
      : SimpleOperator(operator_id, "Limit", OperatorType::Limit),
        remaining_(limit),
        memory_manager_(std::move(memory_manager)) {}

  /// @brief Rows still available to claim (for tests / introspection).
  [[nodiscard]] std::size_t remaining() const noexcept {
    return remaining_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool ShouldStopSource() const noexcept override {
    return remaining_.load(std::memory_order_relaxed) == 0;
  }

 protected:
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override {
    auto host_result = execution::EnsureHostBlock(std::move(input));
    if (!host_result) {
      return std::unexpected(host_result.error());
    }
    auto host = std::move(*host_result);
    const auto* batch = host->record_batch();
    if (!batch) {
      return Error::Invalid("LimitOperator: empty record batch");
    }

    const std::size_t rows = batch->num_rows();
    std::size_t rem = remaining_.load(std::memory_order_relaxed);
    std::size_t take = 0;
    while (true) {
      if (rem == 0) {
        take = 0;
        break;
      }
      take = std::min(rows, rem);
      if (remaining_.compare_exchange_weak(rem, rem - take, std::memory_order_acq_rel,
                                           std::memory_order_relaxed)) {
        break;
      }
    }

    if (take == 0) {
      return execution::MakeTrackedHostBlock(std::format("limit_{}", id()),
                                             datasource::RecordBatch(batch->schema(), {}),
                                             memory_manager_);
    }
    if (take == rows) {
      // Whole batch fits — pass through without re-materializing.
      return host;
    }

    auto truncated = execution::TruncateRecordBatch(*batch, take);
    if (!truncated) {
      return std::unexpected(truncated.error());
    }
    return execution::MakeTrackedHostBlock(std::format("limit_{}", id()), std::move(*truncated),
                                           memory_manager_);
  }

 private:
  std::atomic<std::size_t> remaining_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;
};

}  // namespace quest::taskflow
