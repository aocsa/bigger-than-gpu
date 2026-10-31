// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_TABLE_SCAN_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_TABLE_SCAN_OPERATOR_HPP_

/// @file table_scan_operator.hpp
/// @brief Pipeline-role TableScan that yields HostMemoryBlocks from a DataSource.

#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/execution/batch_kernels.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Source operator: iterates a DataSource and wraps each batch.
class TableScanOperator : public Operator {
 public:
  TableScanOperator(std::uint32_t operator_id, std::shared_ptr<datasource::DataSource> source,
                    std::optional<std::vector<std::string>> projection = std::nullopt,
                    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr)
      : Operator(operator_id, "TableScan", OperatorType::TableScan),
        source_(std::move(source)),
        projection_(std::move(projection)),
        memory_manager_(std::move(memory_manager)) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  /// @brief The columns this scan reads (nullopt = all source columns).
  ///
  /// Set when the gate folds a Projection into the scan (BindableTableScan) —
  /// the folded list has fewer columns than the source, proving column pruning.
  [[nodiscard]] const std::optional<std::vector<std::string>>& projection() const noexcept {
    return projection_;
  }

  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override {
    if (!started_) {
      std::vector<std::string> proj;
      if (projection_) {
        proj = *projection_;
      } else {
        for (const auto& field : source_->schema().fields()) {
          proj.push_back(field.name);
        }
      }
      sequence_ = source_->Scan(proj);
      iter_ = sequence_->begin();
      end_ = sequence_->end();
      started_ = true;
    }

    if (iter_ == end_) {
      return std::optional<cache::DataBlockPtr>{std::nullopt};
    }

    auto batch_result = *iter_;
    ++iter_;
    if (!batch_result) {
      return std::unexpected(batch_result.error());
    }

    return std::optional<cache::DataBlockPtr>{
        execution::MakeTrackedHostBlock(std::format("scan_{}_{}", id(), next_batch_id_++),
                                        std::move(*batch_result), memory_manager_)};
  }

 private:
  std::shared_ptr<datasource::DataSource> source_;
  std::optional<std::vector<std::string>> projection_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  bool started_{false};
  std::optional<datasource::RecordBatchSequence> sequence_;
  datasource::RecordBatchIterator iter_{nullptr, {}, 0};
  datasource::RecordBatchIterator end_{nullptr, {}, 0};
  std::uint64_t next_batch_id_{0};
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_TABLE_SCAN_OPERATOR_HPP_
