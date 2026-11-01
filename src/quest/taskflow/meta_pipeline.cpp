// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/meta_pipeline.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace quest::taskflow {

namespace {

[[nodiscard]] bool HasFusedPort(const Operator& op) {
  return std::ranges::any_of(op.DeclaredInputPorts(), [&op](std::string_view port) {
    return op.InputPortRole(port) == PortRole::kFused;
  });
}

/// @brief Walk state for one MetaPipeline::Build() call.
///
/// Class rather than free functions so the recursive upstream walk can access
/// the pipeline vector and memo at every depth without threading them as params.
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
  /// @brief Returns (or builds and memoizes) the pipeline whose sink is `sink_operator_id`.
  ///
  /// A non-empty `sink_port` names a kSink port: the pipeline ends at that
  /// port and walks only its producers. Memoized per (operator, port), since a
  /// hash join is both the sink of its build pipeline and a fused operator of
  /// its probe pipeline.
  ///
  /// `in_progress_` catches cross-pipeline-boundary cycles; a per-walk visited
  /// set catches streaming cycles inside one fuse walk. Both return Invalid
  /// rather than hanging or overflowing the stack.
  [[nodiscard]] Result<Pipeline*> BuildFrom(std::uint32_t sink_operator_id,
                                            const std::string& sink_port = {}) {
    const SinkKey key{sink_operator_id, sink_port};
    auto memo_it = memo_.find(key);
    if (memo_it != memo_.end()) {
      return memo_it->second;
    }
    if (in_progress_.contains(key)) {
      return Err(ErrorCode::Invalid,
                 "MetaPipeline::Build: cycle detected in operator graph");
    }

    auto* sink_op = graph_.GetNode(sink_operator_id);
    if (sink_op == nullptr) {
      return Err(ErrorCode::Invalid, "MetaPipeline::Build: unknown operator id");
    }

    in_progress_.insert(key);

    auto owned_pipeline = std::make_unique<Pipeline>(next_pipeline_id_++);
    Pipeline* pipeline = owned_pipeline.get();
    pipeline->PrependOperator(sink_op);

    if (sink_port.empty() && HasFusedPort(*sink_op)) {
      // Reached through its fused port, so it streams (ProcessBatch) into a
      // downstream barrier holder instead of accumulating.
      if (graph_.GetOutgoingEdges(sink_op).empty()) {
        return Err(ErrorCode::Invalid,
                   "MetaPipeline::Build: an operator with a fused input port needs a "
                   "downstream consumer");
      }
      pipeline->MarkStreamingSink();
    }

    std::unordered_set<std::uint32_t> walk_visited{sink_operator_id};
    Operator* cur = sink_op;
    while (true) {
      auto incoming = graph_.GetIncomingEdges(cur);
      if (cur == sink_op && !sink_port.empty()) {
        std::erase_if(incoming, [&sink_port](const Edge& edge) {
          return edge.target_port_name != sink_port;
        });
      } else if (std::ranges::any_of(incoming, [cur](const Edge& edge) {
                   return cur->InputPortRole(edge.target_port_name) != PortRole::kHolder;
                 })) {
        auto fused = WalkPortRoleProducers(*cur, incoming, *pipeline);
        if (!fused.has_value()) {
          return std::unexpected(fused.error());
        }
        incoming = std::move(fused.value());
      }
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

    if (HasFusedPort(*pipeline->source())) {
      // Nothing streams into it, so it would have to claim from its input
      // holders, which no pipeline fills: a missing probe producer, or a
      // barrier edge on its fused or sink port.
      return Err(ErrorCode::Invalid,
                 "MetaPipeline::Build: an operator with a fused input port cannot be a "
                 "pipeline source; wire a streaming producer into its fused port and "
                 "its sink ports");
    }

    in_progress_.erase(key);
    pipelines_.push_back(std::move(owned_pipeline));
    memo_[key] = pipeline;
    return pipeline;
  }

  /// @brief Port-role case: builds one child pipeline per kSink port (a Full
  ///        producer of `pipeline`) and returns the edge on the kFused port,
  ///        through which `pipeline` keeps walking upstream (empty if none).
  [[nodiscard]] Result<std::set<Edge>> WalkPortRoleProducers(const Operator& op,
                                                             const std::set<Edge>& incoming,
                                                             Pipeline& pipeline) {
    std::vector<Edge> ordered(incoming.begin(), incoming.end());
    std::ranges::sort(ordered, {}, &Edge::target_port_name);

    std::set<Edge> fused;
    for (const auto& edge : ordered) {
      switch (op.InputPortRole(edge.target_port_name)) {
        case PortRole::kSink: {
          auto child = BuildFrom(op.id(), edge.target_port_name);
          if (!child.has_value()) {
            return std::unexpected(child.error());
          }
          pipeline.AddProducer(child.value(), BarrierType::Full);
          break;
        }
        case PortRole::kFused:
          if (!fused.empty()) {
            return Err(ErrorCode::Invalid,
                       "MetaPipeline::Build: an operator may have at most one fused input");
          }
          fused.insert(edge);
          break;
        case PortRole::kHolder:
          return Err(ErrorCode::Invalid,
                     "MetaPipeline::Build: an operator cannot mix holder ports with sink or "
                     "fused ports");
      }
    }
    return fused;
  }

  /// @brief Single-incoming-edge case.
  ///
  /// Returns nullopt if the edge ended the pipeline (blocking op or explicit
  /// Full/partitioned break); returns the upstream operator if the edge fused
  /// (caller keeps walking); returns an error if the graph is malformed.
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
    if (edge_config.barrier == BarrierType::Full || edge_config.num_partitions > 1 ||
        edge_config.type != cache::BatchHolderType::kSimple) {
      // Explicit cross-pipeline break without a blocking operator:
      // upstream belongs entirely to the child pipeline, not shared.
      // Non-simple holder types (Concatenating, NonWaiting) always break —
      // fusing would bypass the holder whose semantics the edge asked for.
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

  /// @brief Multi-input case: builds one child pipeline per producer in port-name order.
  [[nodiscard]] Status WalkMultiInputProducers(const std::set<Edge>& incoming, Pipeline& pipeline) {
    std::vector<Edge> ordered(incoming.begin(), incoming.end());
    std::ranges::sort(ordered, {}, &Edge::target_port_name);

    for (const auto& edge : ordered) {
      auto* upstream = graph_.GetNode(edge.source_operator_id);
      if (upstream == nullptr) {
        return Error::Invalid(
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
        return Error::Invalid(
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
    return {};
  }

  const Graph& graph_;
  std::vector<PipelinePtr> pipelines_;
  // (sink operator id, sink port); the port is empty unless it is a kSink port.
  using SinkKey = std::pair<std::uint32_t, std::string>;
  std::map<SinkKey, Pipeline*> memo_;
  // Sinks whose BuildFrom() is on the current recursion stack; re-entering
  // one means the graph has a cycle crossing pipeline boundaries.
  std::set<SinkKey> in_progress_;
  std::size_t next_pipeline_id_{0};
};

}  // namespace

Result<std::vector<PipelinePtr>> MetaPipeline::Build(const Graph& graph) {
  return MetaPipelineBuilder(graph).Build();
}

}  // namespace quest::taskflow
