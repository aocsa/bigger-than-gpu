// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file hash_join_operator.hpp
/// @brief Inner equi-join on one integer key: builds a hash table from the
///        "build" port, streams the "probe" port through it.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Inner hash join on one Int32/Int64 key column per side.
///
/// Spans two pipelines. The build pipeline ends at the "build" port (kSink):
/// `Sink()` accumulates its batches and `FinalizeSink()` builds the hash table
/// when that pipeline finishes. The probe pipeline fuses through the "probe"
/// port (kFused): `ProcessBatch()` joins each probe batch. Output rows are the
/// probe columns followed by the build columns, in probe order and, for each
/// probe row, in build insertion order. Null keys never match.
class HashJoinOperator : public Operator {
 public:
  /// @param operator_id Graph-unique operator id.
  /// @param build_key Key column of the build input (Int32 or Int64).
  /// @param probe_key Key column of the probe input (Int32 or Int64).
  /// @param memory_manager Stamped onto output blocks so they stay visible to
  ///        saturation accounting and spillable at pipeline breaks.
  HashJoinOperator(std::uint32_t operator_id, std::string build_key, std::string probe_key,
                   std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  /// @brief Not a sink-to-source breaker: the probe side streams through it.
  [[nodiscard]] bool IsBlocking() const noexcept override { return false; }

  /// @brief The sink role of the build pipeline.
  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] std::vector<std::string_view> DeclaredInputPorts() const override {
    return {"build", "probe"};
  }

  /// @return kSink for "build", kFused for "probe".
  [[nodiscard]] PortRole InputPortRole(std::string_view port) const override;

  /// @brief Typed accessor for the build input port.
  [[nodiscard]] OperatorPortPair build() { return {this, "build"}; }

  /// @brief Typed accessor for the probe input port.
  [[nodiscard]] OperatorPortPair probe() { return {this, "probe"}; }

  /// @brief Appends one build batch.
  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override;

  /// @brief Builds the hash table from the accumulated build batches.
  /// @return SchemaError if the build key column is missing, TypeError if it
  ///         is not Int32/Int64.
  [[nodiscard]] Status FinalizeSink() override;

  /// @brief Joins one probe batch against the finalized build side.
  /// @return The joined rows (zero rows if none match), SchemaError if the
  ///         probe key column is missing, or Invalid before `FinalizeSink()`.
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override;

 private:
  std::string build_key_;
  std::string probe_key_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  std::mutex sink_mutex_;
  std::vector<cache::DataBlockPtr> build_blocks_;  // host blocks, loaded at Sink()

  // Written by FinalizeSink(), read-only afterwards; the executor schedules
  // the probe pipeline only after the build pipeline finalized.
  bool finalized_{false};
  std::optional<columnar::RecordBatch> build_batch_;  // nullopt when the build side is empty
  std::unordered_map<std::int64_t, std::vector<std::size_t>> build_rows_by_key_;
};

}  // namespace quest::taskflow
