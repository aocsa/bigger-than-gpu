// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file filter_operator.hpp
/// @brief Pipeline-role Filter that evaluates a pre-built physical expression.

#include <cstdint>
#include <format>
#include <memory>
#include <utility>

#include "quest/cache/data_block.hpp"
#include "quest/common/result.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Streaming filter: mask + gather inside ProcessBatch.
class FilterOperator : public SimpleOperator {
 public:
  /// @param memory_manager Stamped onto output blocks so they stay visible
  ///        to saturation accounting and spillable at pipeline breaks.
  FilterOperator(std::uint32_t operator_id, plan::physical::ExpressionPtr predicate,
                 std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr)
      : SimpleOperator(operator_id, "Filter", OperatorType::Filter),
        predicate_(std::move(predicate)),
        memory_manager_(std::move(memory_manager)) {}

 protected:
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override {
    auto host_result = cache::EnsureHostBlock(std::move(input));
    if (!host_result) {
      return std::unexpected(host_result.error());
    }
    auto host = std::move(*host_result);
    const auto* batch = host->record_batch();
    if (!batch) {
      return Error::Invalid("FilterOperator: empty record batch");
    }

    auto filtered = execution::FilterRecordBatch(*batch, *predicate_);
    if (!filtered) {
      return std::unexpected(filtered.error());
    }

    return cache::MakeTrackedHostBlock(std::format("filter_{}", id()), std::move(*filtered),
                                           memory_manager_);
  }

 private:
  plan::physical::ExpressionPtr predicate_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;
};

}  // namespace quest::taskflow
