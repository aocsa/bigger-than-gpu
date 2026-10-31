// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_META_PIPELINE_HPP_
#define QUEST_TASKFLOW_META_PIPELINE_HPP_

/// @file meta_pipeline.hpp
/// @brief MetaPipeline::Build — splits a finalized Graph into Pipelines at
///        blocking operators (docs/dev/pipeline-execution-plan.md, §2.4).

#include <vector>

#include "quest/result.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/pipeline.hpp"

namespace quest::taskflow {

/// @brief Builds the set of Pipelines for a Graph.
///
/// Mirrors Sirius's `sirius_meta_pipeline::build()`: streaming operators
/// fuse into the current pipeline; a blocking operator becomes the source
/// of the current pipeline AND the sink of a child pipeline, built first
/// (build side before probe side, matching Sirius's recursion order).
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
  ///         a cycle, or a blocking operator feeding a multi-input
  ///         consumer — unsupported until relay pipelines exist).
  [[nodiscard]] static Result<std::vector<PipelinePtr>> Build(const Graph& graph);
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_META_PIPELINE_HPP_
