// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file meta_pipeline.hpp
/// @brief MetaPipeline::Build — splits a finalized Graph into Pipelines at
///        blocking operators (docs/dev/pipeline-execution-plan.md, §2.4).

#include <vector>

#include "quest/common/result.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/pipeline.hpp"

namespace quest::taskflow {

/// @brief Builds the set of Pipelines for a Graph.
///
/// Mirrors Sirius's `sirius_meta_pipeline::build()`: streaming operators
/// fuse into the current pipeline; a blocking operator becomes the source
/// of the current pipeline AND the sink of a child pipeline, built first
/// (build side before probe side, matching Sirius's recursion order).
/// Operators with port roles (`Operator::InputPortRole`, e.g. a hash join)
/// are split per port instead: each kSink port ends a child pipeline at the
/// operator, and the current pipeline fuses through the kFused port.
class MetaPipeline {
 public:
  MetaPipeline() = delete;

  /// @brief Walks `graph` upstream from every terminal operator and splits
  /// it into pipelines at blocking operators and explicit Full/partitioned
  /// edges.
  ///
  /// Child pipelines (those feeding a `Full`-barrier or blocking boundary)
  /// are always built and appended to the result before the pipeline that
  /// depends on them — a caller can schedule pipelines in vector order and
  /// have producers already resolved.
  ///
  /// @param graph The graph to split. Must outlive the returned pipelines
  ///        (they hold non-owning `Operator*` into it). Terminal operators are
  ///        discovered automatically from graph structure.
  /// @return The pipelines in dependency order, or an error if the graph
  ///         is malformed (an edge referencing an unregistered operator,
  ///         a cycle, a blocking operator feeding a multi-input
  ///         consumer — unsupported until relay pipelines exist — or a
  ///         port-role operator with more than one kFused port, no
  ///         streaming producer on it, or no downstream consumer).
  [[nodiscard]] static Result<std::vector<PipelinePtr>> Build(const Graph& graph);
};

}  // namespace quest::taskflow
