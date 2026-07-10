// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_LIMIT_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_LIMIT_OPERATOR_HPP_

/// @file limit_operator.hpp
/// @brief Streaming Limit that truncates the pipeline to the first N rows.
///
/// v1 truncates downstream via an atomic remaining-rows claim; true source
/// cancellation (stop the scan early) is a phase-3 concern.

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
/// HostMemoryBlocks so the pipeline continues until source EOS (no early
/// source cancellation in v1).
class LimitOperator : public SimpleOperator {
 public:
  /// @param memory_manager Stamped onto output blocks so they stay visible
  ///        to saturation accounting and spillable at pipeline breaks.
  LimitOperator(std::uint32_t operator_id, std::size_t limit,
                std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr)
      : SimpleOperator(operator_id, "Limit", OperatorType::Limit),
        remaining_(limit),
        memory_manager_(std::move(memory_manager)) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  /// @brief Rows still available to claim (for tests / introspection).
  [[nodiscard]] std::size_t remaining() const noexcept {
    return remaining_.load(std::memory_order_relaxed);
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
      return Status::Invalid("LimitOperator: empty record batch");
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

#endif  // QUEST_TASKFLOW_OPERATORS_LIMIT_OPERATOR_HPP_
