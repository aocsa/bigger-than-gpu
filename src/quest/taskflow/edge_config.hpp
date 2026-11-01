// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file edge_config.hpp
/// @brief EdgeConfig, BarrierType, and SpillPolicy for graph edge semantics.
///
/// Extracted from kpair.hpp (D2) so that GraphBuilder and Operator can depend
/// on edge configuration types without pulling in the legacy KPair DSL.
/// kpair.hpp and its DSL (>>, link(), Graph::operator+=) were removed in D4.

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string_view>
#include <utility>

#include "quest/cache/batch_holder.hpp"

namespace quest::taskflow {

// =============================================================================
// Barrier Semantics
// =============================================================================

/// @brief Barrier semantics for an edge between two pipelines.
///
/// Mirrors Sirius's MemoryBarrierType. The barrier is a property of the
/// consumer side of a cross-pipeline edge and gates when the consumer may
/// start consuming:
/// - Pipeline: pure streaming — the consumer may run while the producer
///   pipeline is still active (batches flow through the BatchHolder).
/// - Full: the consumer waits until the entire producer pipeline has
///   finished before consuming any buffered data (hash-join build sides,
///   merge stages, concatenating sinks).
///
/// A Partial barrier (incremental consumption with coordination) is
/// deliberately not modeled yet; it arrives with partitioned joins.
enum class BarrierType : std::uint8_t {
  Pipeline,
  Full
};

inline constexpr std::array<std::string_view, 2> kBarrierTypeNames = {"Pipeline", "Full"};
static_assert(kBarrierTypeNames.size() == std::to_underlying(BarrierType::Full) + 1);

[[nodiscard]] constexpr std::string_view to_string(BarrierType type) noexcept {
  return kBarrierTypeNames[std::to_underlying(type)];
}

// =============================================================================
// Spill Policy
// =============================================================================

/// @brief Eligibility/priority hint for spill candidate selection.
///
/// Phase-stable semantics: today `Never` maps to `auto_spill=false` on the
/// holder. After phase 5 this feeds the downgrade executor's candidate
/// enumeration: Never = skipped, First = front of the victim order.
enum class SpillPolicy : std::uint8_t {
  Eligible,  ///< Default: this edge's buffered batches may spill.
  Never,     ///< Exempt from spilling (latency-critical, tiny).
  First      ///< Preferred spill victim (large, cold — e.g. a join build side).
};

// =============================================================================
// Edge Configuration
// =============================================================================

/// @brief Sentinel for "unlimited" flow-control threshold.
static constexpr std::size_t kUnlimited = std::numeric_limits<std::size_t>::max();

/// @brief Configuration for the batch holder connection between two operators.
///
/// EdgeConfig controls how data flows between operators:
/// - Type: Simple (individual batches) or Concatenating (merged batches)
/// - Barrier: Pipeline (streaming) or Full (wait for producer pipeline)
/// - Partitioning: Fixed count (num_partitions) or late-bound upper-bound
///   (max_partitions > 0 means PartitionedUpTo — the operator binds the
///   actual count ≤ max at runtime; phase 6 mechanism, stored here in D2).
/// - Flow control: Backpressure thresholds to prevent memory exhaustion
/// - Spill: eligibility hint for the downgrade executor (phase 5 mechanism)
struct EdgeConfig {
  /// Type of batch holder (affects how data is delivered to consumers).
  cache::BatchHolderType type{cache::BatchHolderType::kSimple};

  /// Barrier semantics for pipeline execution (see BarrierType).
  BarrierType barrier{BarrierType::Pipeline};

  /// Fixed partition count for parallel processing patterns.
  /// Values > 1 create multiple batch holders (FOR_EACH pattern).
  std::size_t num_partitions{1};

  /// Upper bound for late-bound partition count (PartitionedUpTo semantics).
  /// 0 = not set (use num_partitions as fixed count).
  /// Phase 6 binds the actual count at runtime within this bound.
  std::size_t max_partitions{0};

  /// Maximum number of batches before applying backpressure (unlimited by default).
  std::size_t flow_control_batches_threshold{kUnlimited};

  /// Maximum bytes before applying backpressure (unlimited by default).
  std::size_t flow_control_bytes_threshold{kUnlimited};

  /// Spill eligibility hint for the downgrade executor.
  SpillPolicy spill_policy{SpillPolicy::Eligible};

  /// Concatenating edges only: merge into chunks of up to this many bytes per
  /// delivered batch instead of one all-encompassing block (0 = concat-all).
  /// BlazingSQL heritage (mature concat byte cap / Sirius _concat_batch_bytes);
  /// stats-tunable via the policy chain.
  std::size_t concat_bytes_threshold{0};

  // ---- Factory helpers -------------------------------------------------------

  /// @brief Implies Full barrier: merging requires the producer to finish first.
  [[nodiscard]] static EdgeConfig Concatenating() {
    return EdgeConfig{.type = cache::BatchHolderType::kConcatenating,
                      .barrier = BarrierType::Full};
  }

  [[nodiscard]] static EdgeConfig Partitioned(std::size_t partition_count) {
    return EdgeConfig{.type = cache::BatchHolderType::kSimple,
                      .num_partitions = partition_count};
  }

  // ---- Mutating builder methods (for legacy DSL and GraphBuilder internals) --

  EdgeConfig& with_barrier(BarrierType barrier_type) {
    barrier = barrier_type;
    return *this;
  }

  EdgeConfig& with_flow_control(
      std::size_t max_batches,
      std::size_t max_bytes = kUnlimited) {
    flow_control_batches_threshold = max_batches;
    flow_control_bytes_threshold = max_bytes;
    return *this;
  }
};

}  // namespace quest::taskflow
