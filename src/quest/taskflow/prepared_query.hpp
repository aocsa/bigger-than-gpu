// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_PREPARED_QUERY_HPP_
#define QUEST_TASKFLOW_PREPARED_QUERY_HPP_

/// @file prepared_query.hpp
/// @brief PreparedQuery — holders + pipelines + executor after Graph::Prepare().
///
/// A Graph in the Built state stores edge specs only (no holders). Prepare()
/// materializes holders, builds pipelines, and hands them to a PreparedQuery.
/// Soft knobs (flow control, spill policy, ScaleFlowControl) live here;
/// structural retuning stays on Graph.

#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/edge_ref.hpp"
#include "quest/taskflow/pipeline.hpp"

namespace quest::taskflow {

class PipelineExecutor;

/// @brief Per-edge metadata carried alongside materialized holders.
///
/// One PreparedEdge may own multiple holders when `num_partitions > 1`.
struct PreparedEdge {
  EdgeInfo info;
  std::vector<std::shared_ptr<cache::IBatchHolder>> holders;
};

/// @brief Executable query produced by Graph::Prepare(stats, hw).
///
/// Owns the materialized holders (authoritative lifetime), the pipelines, and
/// the PipelineExecutor used by Execute(). Movable; single-Execute — re-run by
/// calling Graph::Prepare() again.
class PreparedQuery {
 public:
  PreparedQuery(std::vector<PreparedEdge> edges, std::vector<PipelinePtr> pipelines);

  ~PreparedQuery();

  PreparedQuery(PreparedQuery&&) noexcept;
  PreparedQuery& operator=(PreparedQuery&&) noexcept;

  PreparedQuery(const PreparedQuery&) = delete;
  PreparedQuery& operator=(const PreparedQuery&) = delete;

  /// @brief Runs the prepared pipelines to completion via PipelineExecutor.
  [[nodiscard]] Status Execute();

  /// @brief FINAL edge view (synthesized edges included once phase 6 lands).
  [[nodiscard]] std::vector<EdgeInfo> Edges() const;

  /// @brief Live soft knob: raise/lower flow-control thresholds on one edge.
  ///
  /// Updates every partition holder and wakes producers blocked in
  /// WaitIfSaturated(). Phase 3 will also post a task-re-evaluation event.
  [[nodiscard]] Status SetFlowControl(const EdgeRef& ref, std::size_t max_batches,
                                      std::size_t max_bytes);

  /// @brief Live soft knob: SpillPolicy::Never → auto_spill=false; else true.
  [[nodiscard]] Status SetSpillPolicy(const EdgeRef& ref, SpillPolicy policy);

  /// @brief Multiplies every edge's FC thresholds by `factor` (clamped ≥1 batch).
  ///
  /// Documented stopgap: phase 5's downgrade executor is the principled
  /// pressure-relief mechanism.
  [[nodiscard]] Status ScaleFlowControl(double factor);

  /// @brief Reserved until phase 3 (task-creator admission). Always Invalid.
  [[nodiscard]] Status SetMaxInFlightTasks(std::size_t pipeline_id, std::size_t n);

  /// @brief Number of materialized holders (one per partition per edge).
  [[nodiscard]] std::size_t holder_count() const noexcept;

  /// @brief Number of pipelines built by MetaPipeline::Build.
  [[nodiscard]] std::size_t pipeline_count() const noexcept { return pipelines_.size(); }

  /// @brief Non-owning view of the prepared pipelines (valid until Execute()).
  [[nodiscard]] const std::vector<PipelinePtr>& pipelines() const noexcept { return pipelines_; }

  /// @brief Flat holder list (for tests / Materialize parity).
  [[nodiscard]] std::vector<std::shared_ptr<cache::IBatchHolder>> holders() const;

 private:
  [[nodiscard]] PreparedEdge* FindEdge(const EdgeRef& ref);
  [[nodiscard]] const PreparedEdge* FindEdge(const EdgeRef& ref) const;

  std::vector<PreparedEdge> edges_;
  std::vector<PipelinePtr> pipelines_;
  std::unique_ptr<PipelineExecutor> executor_;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_PREPARED_QUERY_HPP_
