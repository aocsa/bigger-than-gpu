// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file sort_operator.hpp
/// @brief Blocking ORDER BY: buffers all input, emits one stably sorted batch.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief One ORDER BY key.
struct SortKey {
  std::string column;
  bool ascending = true;
};

/// @brief Blocking sort: sink of the producer pipeline, source of the consumer.
///
/// Stable: rows with equal keys keep their input (Sink arrival) order. Numbers compare
/// numerically, strings bytewise. Nulls sort last in both directions (DuckDB's default
/// NULLS LAST for ASC and DESC).
class SortOperator : public Operator {
 public:
  /// @param keys Sort keys, most significant first.
  /// @param memory_manager Stamped onto the output block (may be null).
  SortOperator(std::uint32_t operator_id, std::vector<SortKey> keys,
               std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  /// @brief Buffers @p batch; thread-safe.
  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override;

  /// @brief Concatenates and sorts the buffered rows; must be called exactly once.
  /// @return SchemaError if a sort key names a missing column.
  [[nodiscard]] Status FinalizeSink() override;

  /// @brief True only after FinalizeSink().
  [[nodiscard]] bool IsSource() const noexcept override { return finalized_.load(); }

  /// @brief Emits the sorted batch once (nothing for empty input), then end-of-stream.
  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override;

 private:
  std::vector<SortKey> keys_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  std::mutex mutex_;
  std::vector<cache::DataBlockPtr> batches_;
  std::optional<cache::DataBlockPtr> output_;
  std::atomic<bool> finalized_{false};
};

}  // namespace quest::taskflow
