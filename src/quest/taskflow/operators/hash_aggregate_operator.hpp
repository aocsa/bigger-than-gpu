// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file hash_aggregate_operator.hpp
/// @brief Blocking hash GROUP BY with SUM/COUNT/COUNT(*)/MIN/MAX/AVG.

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Aggregate function applied per group.
enum class AggregateKind : std::uint8_t { kSum, kCount, kCountStar, kMin, kMax, kAvg };

/// @brief One aggregate output column.
struct AggregateSpec {
  AggregateKind kind;
  plan::physical::ExpressionPtr input;  ///< nullptr for kCountStar
  std::string output_name;
};

/// @brief Blocking hash aggregate: sink of the producer pipeline, source of the consumer.
///
/// Output is one batch: the group-by columns (input types) followed by one column per
/// aggregate. Semantics follow DuckDB: SUM/MIN/MAX/AVG/COUNT(expr) ignore nulls and return
/// null (COUNT: 0) for a group without non-null inputs; SUM of integers is Int64 and an
/// overflow is an OutOfRange error; SUM of floats and AVG are Double; COUNT is Int64;
/// MIN/MAX keep the input type. Null group keys form one group. Without GROUP BY the output
/// is exactly one row, even for empty input; with GROUP BY, empty input emits nothing.
/// Groups are emitted in first-appearance order of the Sink() merges.
///
/// When no batch was ever sunk, aggregate input types are unknown, so SUM/MIN/MAX fall
/// back to an Int64 null column.
class HashAggregateOperator : public Operator {
 public:
  /// @param group_by Input column names to group by (empty: one global group).
  /// @param aggregates Aggregates evaluated per batch against the input.
  /// @param memory_manager Stamped onto the output block (may be null).
  HashAggregateOperator(std::uint32_t operator_id, std::vector<std::string> group_by,
                        std::vector<AggregateSpec> aggregates,
                        std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  /// @brief Pre-aggregates @p batch locally, then merges it into the shared table.
  ///
  /// Thread-safe: several tasks of the producer pipeline may call it concurrently.
  /// @return SchemaError for a missing group column, TypeError for SUM/AVG over a
  ///         non-numeric input, OutOfRange on integer SUM overflow.
  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override;

  /// @brief Builds the output batch; must be called exactly once, after the last Sink().
  [[nodiscard]] Status FinalizeSink() override;

  /// @brief True only after FinalizeSink().
  [[nodiscard]] bool IsSource() const noexcept override { return finalized_.load(); }

  /// @brief Emits the aggregated batch once, then end-of-stream.
  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override;

 private:
  /// @brief Per-group running state of one aggregate.
  struct AggregateState {
    std::int64_t count = 0;  ///< non-null inputs (rows for COUNT(*))
    std::int64_t int_sum = 0;
    double double_sum = 0.0;
    columnar::Value extreme;  ///< MIN/MAX so far; monostate while unset
  };

  /// @brief One group: its key values and one state per aggregate.
  struct Group {
    std::vector<columnar::Value> key;
    std::vector<AggregateState> states;
  };

  /// @brief Hash of a group key (vector of Values).
  struct KeyHash {
    [[nodiscard]] std::size_t operator()(const std::vector<columnar::Value>& key) const noexcept;
  };

  /// @brief Groups in first-appearance order plus a key -> position index.
  struct GroupTable {
    std::vector<Group> groups;
    std::unordered_map<std::vector<columnar::Value>, std::size_t, KeyHash> index;
  };

  [[nodiscard]] Result<columnar::RecordBatch> BuildOutput() const;

  std::vector<std::string> group_by_;
  std::vector<AggregateSpec> aggregates_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  mutable std::mutex mutex_;
  GroupTable table_;
  // Resolved from the first non-empty batch.
  std::optional<std::vector<columnar::Field>> group_fields_;
  std::vector<std::optional<columnar::DataType>> input_types_;
  std::optional<cache::DataBlockPtr> output_;
  std::atomic<bool> finalized_{false};
};

}  // namespace quest::taskflow
