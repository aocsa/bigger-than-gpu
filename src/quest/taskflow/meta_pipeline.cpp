// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/meta_pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quest::taskflow {

namespace {

/// @brief Walk state for one MetaPipeline::Build() call.
///
/// Not exposed: MetaPipeline::Build() is the only entry point. Kept as a
/// class (rather than free functions threading state through parameters)
/// because the upstream walk is recursive and needs the output vector plus
/// the sink-id -> Pipeline* memo available at every recursion depth.
class MetaPipelineBuilder {
 public:
  explicit MetaPipelineBuilder(const Graph& graph) : graph_(graph) {}

  [[nodiscard]] Result<std::vector<PipelinePtr>> Build() {
    for (auto* terminal : graph_.GetTerminalOperators()) {
      auto result = BuildFrom(terminal->id());
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
    }
    return std::move(pipelines_);
  }

 private:
  /// @brief Builds (or returns the memoized) pipeline whose sink is the
  /// operator identified by `sink_operator_id`.
  ///
  /// Guards against cyclic graphs (the DSL never validates acyclicity):
  /// `in_progress_` catches cycles that cross pipeline boundaries (the
  /// recursion re-entering a sink it is already building), and a per-walk
  /// visited set catches pure streaming cycles inside one fuse walk.
  /// Either returns Invalid instead of hanging or overflowing the stack.
  [[nodiscard]] Result<Pipeline*> BuildFrom(std::uint32_t sink_operator_id) {
    auto memo_it = memo_.find(sink_operator_id);
    if (memo_it != memo_.end()) {
      return memo_it->second;
    }
    if (in_progress_.contains(sink_operator_id)) {
      return Err(ErrorCode::Invalid,
                 "MetaPipeline::Build: cycle detected in operator graph");
    }

    auto* sink_op = graph_.GetNode(sink_operator_id);
    if (sink_op == nullptr) {
      return Err(ErrorCode::Invalid, "MetaPipeline::Build: unknown operator id");
    }

    in_progress_.insert(sink_operator_id);

    auto owned_pipeline = std::make_unique<Pipeline>(next_pipeline_id_++);
    Pipeline* pipeline = owned_pipeline.get();
    pipeline->PrependOperator(sink_op);

    std::unordered_set<std::uint32_t> walk_visited{sink_operator_id};
    Operator* cur = sink_op;
    while (true) {
      auto incoming = graph_.GetIncomingEdges(cur);
      if (incoming.empty()) {
        break;  // cur has no producer: it is a true source.
      }

      if (incoming.size() == 1) {
        auto walk_result = WalkSingleProducer(*incoming.begin(), *pipeline);
        if (!walk_result.has_value()) {
          return std::unexpected(walk_result.error());
        }
        if (!walk_result.value().has_value()) {
          break;  // the edge ended this pipeline (Full or partitioned break).
        }
        cur = walk_result.value().value();  // fused: keep walking upstream.
        if (!walk_visited.insert(cur->id()).second) {
          return Err(ErrorCode::Invalid,
                     "MetaPipeline::Build: cycle detected in operator graph");
        }
        continue;
      }

      // Multi-input operator (join-shaped): one child pipeline per input
      // port. Edge's set order is (target, source), not port name, so
      // sort explicitly for deterministic, reviewable output.
      auto multi_result = WalkMultiInputProducers(incoming, *pipeline);
      if (!multi_result.has_value()) {
        return std::unexpected(multi_result.error());
      }
      break;
    }

    in_progress_.erase(sink_operator_id);
    pipelines_.push_back(std::move(owned_pipeline));
    memo_[sink_operator_id] = pipeline;
    return pipeline;
  }

  /// @brief Handles the single-incoming-edge case for `cur` in `pipeline`.
  ///
  /// @return An engaged-but-empty optional if the edge ended the pipeline
  ///         (upstream became a child via a blocking op or an explicit
  ///         Full/partitioned break); an optional holding the upstream
  ///         operator if the edge fused (caller should keep walking from
  ///         it); an error if the graph is malformed.
  [[nodiscard]] Result<std::optional<Operator*>> WalkSingleProducer(const Edge& edge,
                                                                    Pipeline& pipeline) {
    auto* upstream = graph_.GetNode(edge.source_operator_id);
    if (upstream == nullptr) {
      return Err(ErrorCode::Invalid,
                 "MetaPipeline::Build: edge references unregistered "
                 "source operator");
    }

    if (upstream->IsBlocking()) {
      // Sirius invariant: a blocking operator is the sink of its own
      // (child) pipeline and the source of the pipeline it feeds.
      pipeline.PrependOperator(upstream);
      auto child = BuildFrom(upstream->id());
      if (!child.has_value()) {
        return std::unexpected(child.error());
      }
      pipeline.AddProducer(child.value(), BarrierType::Full);
      return std::optional<Operator*>{std::nullopt};
    }

    auto edge_config = graph_.GetEdgeConfig(edge.source_operator_id, edge.target_operator_id);
    if (edge_config.barrier == BarrierType::Full || edge_config.num_partitions > 1) {
      // Explicit cross-pipeline break without a blocking operator:
      // upstream belongs entirely to the child pipeline, not shared.
      auto child = BuildFrom(upstream->id());
      if (!child.has_value()) {
        return std::unexpected(child.error());
      }
      pipeline.AddProducer(child.value(), edge_config.barrier);
      return std::optional<Operator*>{std::nullopt};
    }

    // Pure streaming edge: fuse into the current pipeline.
    pipeline.PrependOperator(upstream);
    return std::optional<Operator*>{upstream};
  }

  /// @brief Handles the multi-input case: builds one child pipeline per
  /// producer, in target-port-name order, and records each as a producer
  /// of `pipeline`.
  [[nodiscard]] Status WalkMultiInputProducers(const std::set<Edge>& incoming, Pipeline& pipeline) {
    std::vector<Edge> ordered(incoming.begin(), incoming.end());
    std::ranges::sort(ordered, {}, &Edge::target_port_name);

    for (const auto& edge : ordered) {
      auto* upstream = graph_.GetNode(edge.source_operator_id);
      if (upstream == nullptr) {
        return Status::Invalid(
            "MetaPipeline::Build: edge references unregistered source "
            "operator");
      }
      if (upstream->IsBlocking()) {
        // A blocking producer's child pipeline ends AT the producer, which
        // accumulates via Sink() — but the multi-input path has no relay
        // pipeline to re-emit that state into this consumer's input port
        // (the single-input path solves it by making the blocking op the
        // parent's source). Building the shape anyway would strand the
        // producer's output at execution time, so fail loudly until the
        // relay exists (planned with real joins; see
        // docs/dev/pipeline-execution-plan.md §6 Non-goals).
        return Status::Invalid(
            "MetaPipeline::Build: blocking operator feeding a multi-input "
            "consumer is not supported yet (missing relay pipeline)");
      }
      auto child = BuildFrom(upstream->id());
      if (!child.has_value()) {
        return std::unexpected(child.error());
      }
      auto edge_config = graph_.GetEdgeConfig(edge.source_operator_id, edge.target_operator_id);
      pipeline.AddProducer(child.value(), edge_config.barrier);
    }
    return Status::OK();
  }

  const Graph& graph_;
  std::vector<PipelinePtr> pipelines_;
  std::unordered_map<std::uint32_t, Pipeline*> memo_;
  // Sinks whose BuildFrom() is on the current recursion stack; re-entering
  // one means the graph has a cycle crossing pipeline boundaries.
  std::unordered_set<std::uint32_t> in_progress_;
  std::size_t next_pipeline_id_{0};
};

}  // namespace

Result<std::vector<PipelinePtr>> MetaPipeline::Build(const Graph& graph) {
  return MetaPipelineBuilder(graph).Build();
}

}  // namespace quest::taskflow
