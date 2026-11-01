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
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/plan/aggregate.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief One aggregate output column.
struct AggregateSpec {
  plan::AggregateKind kind;
  plan::physical::ExpressionPtr input;  ///< nullptr for kCountStar
  std::string output_name;
  /// The planner's output type; it types the output when no input row arrives.
  std::optional<columnar::DataType> output_type;
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
/// When no batch was ever sunk, aggregate input types are unknown; the output then takes
/// `AggregateSpec::output_type` (the planner's type), or Int64 without one.
///
/// A group key is its key cells encoded as bytes (a validity byte, then the value; a String
/// has a length prefix), so equal keys are equal bytes: -0.0 and 0.0, and every NaN, share a
/// group. States are arrays indexed by group id, filled from typed views.
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

  /// @brief MIN or MAX of one aggregate for every group, in the input's C++ type.
  class ExtremesBase {
   public:
    virtual ~ExtremesBase() = default;
    /// @brief Folds row r of @p input into group `group_of_row[r]`.
    virtual void Update(const columnar::ColumnVector& input,
                        std::span<const std::uint32_t> group_of_row) = 0;
    /// @brief Folds group g of @p other (same type) into group `remap[g]`.
    virtual void Merge(const ExtremesBase& other, std::span<const std::uint32_t> remap) = 0;
    /// @brief One row per group, null for a group that saw no value.
    [[nodiscard]] virtual columnar::ColumnVectorPtr Finish(columnar::DataType type,
                                                           std::size_t groups) const = 0;
  };

 private:

  /// @brief One aggregate's state for every group, indexed by group id.
  struct States {
    std::vector<std::int64_t> count;  ///< non-null inputs (rows for COUNT(*))
    std::vector<std::int64_t> int_sum;
    std::vector<double> double_sum;
    std::unique_ptr<ExtremesBase> extremes;  ///< MIN/MAX, made with the first input
  };

  /// @brief Groups in first-appearance order: encoded keys, a key -> id index, the states.
  struct GroupTable {
    std::vector<std::string> keys;
    std::unordered_map<std::string, std::uint32_t> index;
    std::vector<States> states;  ///< one per aggregate

    /// @brief The id of @p key, adding a group with zeroed states if it is new.
    std::uint32_t FindOrInsert(const std::string& key);
  };

  [[nodiscard]] static std::unique_ptr<ExtremesBase> MakeExtremes(plan::AggregateKind kind,
                                                                  columnar::DataType type);

  /// @brief Folds @p local into the shared table; the caller holds the mutex.
  [[nodiscard]] Status MergeLocked(const GroupTable& local);

  [[nodiscard]] columnar::RecordBatch BuildOutput() const;

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
