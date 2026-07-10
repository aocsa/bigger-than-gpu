// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_GRAPH_BUILDER_HPP_
#define QUEST_TASKFLOW_GRAPH_BUILDER_HPP_

/// @file graph_builder.hpp
/// @brief GraphBuilder — fluent DSL for wiring execution graphs (Phase 2.5 D2).
///
/// GraphBuilder is the single supported API for constructing execution graphs
/// after Phase 2.5. It replaces the legacy `>>` / `link()` / `Graph::operator+=`
/// surface (deleted in D4).
///
/// Lifecycle:
/// @code
///   GraphBuilder gb{memory_manager};
///   gb.Chain(scan, filter, project, collector);   // Full inferred from contract
///   ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
///   ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(stats, hw));
///   ASSERT_OK(query.Execute());
/// @endcode
///
/// See docs/dev/graph-builder-dsl.md for the full API reference, lifecycle
/// model, config-resolution precedence, and migration gallery.

#include <array>
#include <deque>
#include <map>
#include <memory>
#include <string>
#include <utility>

#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

// Forward declarations for the builder trio.
class EdgeBuilder;
class ChainBuilder;

// =============================================================================
// GraphBuilder
// =============================================================================

/// @brief Fluent builder for constructing and validating execution graphs.
///
/// All operators are non-owning `Operator*` into caller-owned storage, exactly
/// like `Graph` today. `Build()` documents the lifetime contract.
///
/// EdgeBuilder objects returned by `Edge()` and `Chain()` hold a pointer into
/// the builder's internal edge-spec storage. They are intended as short-lived
/// temporaries in fluent chains; do not store them across `Build()`.
class GraphBuilder {
 public:
  // ===========================================================================
  // Internal edge spec (storage for partially-built edges)
  // ===========================================================================

  struct EdgeSpec {
    Operator* source{nullptr};
    std::string source_port;   ///< "" = default (resolved to operator-id at Build)
    Operator* target{nullptr};
    std::string target_port;   ///< "" = default; validated against DeclaredInputPorts
    EdgeConfig config;         ///< Accumulated from author calls
    std::string name;          ///< Set by Named(); stable handle for tuning
    bool explicit_barrier{false};        ///< Full()/Streaming() was called
    bool explicit_type{false};           ///< Concatenating()/ConcatenatingUpTo()/NonWaiting()
    bool explicit_partitions{false};     ///< Partitioned()/PartitionedUpTo() was called
    bool explicit_flow_control{false};   ///< FlowControl() was called
    bool explicit_spill{false};          ///< Spill() was called
    bool config_replaced{false};         ///< Config() (full override) was called
    bool partial_requested{false};       ///< Partial() was called (rejected in Build)
  };

  // ===========================================================================
  // Construction
  // ===========================================================================

  /// @brief Constructs an empty builder.
  ///
  /// @param memory_manager Optional memory manager forwarded to the resulting
  ///        Graph. If null, the Graph uses its own default.
  explicit GraphBuilder(
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  // Non-copyable (edge spec pointers would dangle after copy).
  GraphBuilder(const GraphBuilder&) = delete;
  GraphBuilder& operator=(const GraphBuilder&) = delete;

  // Movable.
  GraphBuilder(GraphBuilder&&) noexcept = default;
  GraphBuilder& operator=(GraphBuilder&&) noexcept = default;

  ~GraphBuilder() = default;

  // ===========================================================================
  // Chain — 90% case (§3.2)
  // ===========================================================================

  /// @brief Wires plain (streaming) edges between consecutive operators.
  ///
  /// `Chain(a, b, c)` creates `Edge(a,b)` + `Edge(b,c)`. The returned
  /// `ChainBuilder` exposes the full EdgeBuilder vocabulary applied to the
  /// **last edge only**:
  /// @code
  ///   gb.Chain(scan, filter, project, collector).Full();
  ///   // project→collector gets Full; scan→filter and filter→project are Streaming
  /// @endcode
  ///
  /// @tparam Ops  `Operator&` or `OperatorPortPair` (any mix).
  template <typename... Ops>
  ChainBuilder Chain(Ops&... ops);

  // ===========================================================================
  // Edge — single edge with fluent config (§3.3)
  // ===========================================================================

  /// @brief Starts (or amends) one edge. The returned EdgeBuilder mutates the
  ///        edge spec in place — no commit step needed.
  EdgeBuilder Edge(Operator& src, Operator& dst);
  EdgeBuilder Edge(Operator& src, OperatorPortPair dst);
  EdgeBuilder Edge(OperatorPortPair src, Operator& dst);
  EdgeBuilder Edge(OperatorPortPair src, OperatorPortPair dst);

  // ===========================================================================
  // Composite shapes — extension point (§3.4)
  // ===========================================================================

  /// @brief Applies a reusable topology pattern.
  ///
  /// Any type with `void WireInto(GraphBuilder&)` works; shapes ship with
  /// their operators, not inside the builder.
  template <typename Shape>
  GraphBuilder& Apply(Shape&& shape) {
    std::forward<Shape>(shape).WireInto(*this);
    return *this;
  }

  // ===========================================================================
  // Build (§3.5)
  // ===========================================================================

  /// @brief Validates and produces the Graph. Consumes the builder.
  ///
  /// Validation (returns an error Status on failure):
  /// (a) Declared port names: typos like `join["biuld"]` become errors, not
  ///     silent new ports.
  /// (b) Acyclicity: any cycle is rejected.
  /// (c) Contract conflicts: explicit barrier contradicts the consumer's
  ///     InputEdgeContract unless `.Config()` was used as a full override.
  /// (d) Duplicate edge names (`.Named()`): rejected.
  /// (e) `.Partial()` is reserved for phase 6 and rejected until it lands.
  ///
  /// The result is a Graph in the Built state: edge specs recorded, NO holders
  /// created. Holders materialize in `Prepare()`.
  [[nodiscard]] Result<std::shared_ptr<Graph>> Build() &&;

  // ===========================================================================
  // Internal helpers (public for EdgeBuilder access)
  // ===========================================================================

  /// @brief Registers a new edge spec and returns a reference to it.
  ///
  /// The reference is stable (deque storage) while the builder is alive.
  EdgeSpec& AddEdgeSpec(Operator* src, std::string src_port,
                        Operator* tgt, std::string tgt_port);

 private:
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  /// Stable storage: deque so raw pointers into elements stay valid as edges grow.
  std::deque<EdgeSpec> edge_specs_;

  /// Tracks Named() edges for duplicate detection at Build() time.
  std::map<std::string, std::size_t> named_edges_;  ///< name → edge_specs_ index
};

// =============================================================================
// EdgeBuilder  (§3.3)
// =============================================================================

/// @brief Fluent configurator for a single edge spec.
///
/// Returned by `GraphBuilder::Edge()` and `GraphBuilder::Chain()`. Holds a
/// raw pointer to the edge spec inside the GraphBuilder; intended as a
/// short-lived temporary in fluent call chains.
class EdgeBuilder {
 public:
  EdgeBuilder(GraphBuilder& builder, GraphBuilder::EdgeSpec* spec) noexcept
      : builder_(&builder), spec_(spec) {}

  // --- barrier semantics (§3.3) ----------------------------------------------

  /// @brief Consumer waits for the full producer pipeline before consuming.
  EdgeBuilder& Full() noexcept;

  /// @brief Pure streaming — consumer and producer run concurrently (default).
  EdgeBuilder& Streaming() noexcept;

  /// @brief RESERVED for phase 6 (partitioned incremental consumption).
  ///
  /// Build() returns an error if this is set: BarrierType::Partial does not
  /// exist yet.
  EdgeBuilder& Partial() noexcept;

  // --- holder type / partitioning (§3.3) ------------------------------------

  /// @brief Merge-all holder; implies Full().
  EdgeBuilder& Concatenating() noexcept;

  /// @brief Chunked concatenation: drain-and-merge up to `bytes` per
  ///        delivered batch (implies Full, like Concatenating). BlazingSQL's
  ///        mature concat byte cap — stats-tunable via the policy chain.
  EdgeBuilder& ConcatenatingUpTo(std::size_t bytes) noexcept;

  /// @brief Non-blocking single-shot holder (BlazingSQL NonWaitingCacheMachine).
  ///
  /// For PRE-FILLED handoffs: the consumer never blocks; empty == drained.
  /// Build() rejects explicit FlowControl()/Full() on these edges — a
  /// pre-filled edge has no producer to throttle and no pipeline to wait on.
  EdgeBuilder& NonWaiting() noexcept;

  /// @brief Fixed partition count, resolved at Prepare().
  EdgeBuilder& Partitioned(std::size_t partition_count) noexcept;

  /// @brief Late-bound upper bound: the operator binds the actual count
  ///        (≤ max_partitions) at runtime when input sizes are known (phase 6).
  EdgeBuilder& PartitionedUpTo(std::size_t max_partitions) noexcept;

  // --- backpressure & spilling (§3.3) ----------------------------------------

  /// @brief Admission condition on the edge's buffer.
  ///
  /// Phase-stable semantics: "don't admit more producer work while the
  /// holder holds ≥ max_batches / max_bytes". Today blocks the producer in
  /// WaitIfSaturated(); after phase 3 gates task creation instead.
  EdgeBuilder& FlowControl(std::size_t max_batches,
                           std::size_t max_bytes = kUnlimited) noexcept;

  /// @brief Eligibility/priority hint for spilling (phase 5 mechanism).
  EdgeBuilder& Spill(SpillPolicy policy) noexcept;

  // --- identity & escape hatch (§3.3) ----------------------------------------

  /// @brief Assigns a stable handle for post-Build tuning (Graph::Tune()).
  EdgeBuilder& Named(std::string edge_name);

  /// @brief Replaces the entire config, bypassing contract conflict checks.
  EdgeBuilder& Config(EdgeConfig config) noexcept;

  // --- continuation (§3.3) ---------------------------------------------------

  /// @brief Creates a new edge from the current target to `next`.
  EdgeBuilder ThenTo(Operator& next);
  EdgeBuilder ThenTo(OperatorPortPair next);

  /// @brief Explicit return to the parent GraphBuilder (optional sugar).
  GraphBuilder& Done() noexcept { return *builder_; }

 protected:
  GraphBuilder* builder_;
  GraphBuilder::EdgeSpec* spec_;
};

// =============================================================================
// ChainBuilder  (§3.2)
// =============================================================================

/// @brief ChainBuilder — EdgeBuilder bound to the chain's last edge.
///
/// Returned by `GraphBuilder::Chain()`. All EdgeBuilder vocabulary applies to
/// the last edge of the chain only.
class ChainBuilder : public EdgeBuilder {
 public:
  using EdgeBuilder::EdgeBuilder;
};

// =============================================================================
// Chain() template implementation (must be in header due to template)
// =============================================================================

namespace detail {

/// @brief Extracts a stable (Operator*, port_name) pair from either type.
inline std::pair<Operator*, std::string> ToEndpoint(Operator& oper) {
  return {&oper, ""};
}
inline std::pair<Operator*, std::string> ToEndpoint(OperatorPortPair& pair) {
  return pair;
}

}  // namespace detail

template <typename... Ops>
ChainBuilder GraphBuilder::Chain(Ops&... ops) {
  static_assert(sizeof...(Ops) >= 2, "Chain requires at least 2 operators");

  auto endpoints = std::array<std::pair<Operator*, std::string>, sizeof...(Ops)>{
      detail::ToEndpoint(ops)...};

  EdgeSpec* last_spec = nullptr;
  for (std::size_t i = 0; i + 1 < endpoints.size(); ++i) {
    last_spec = &AddEdgeSpec(endpoints[i].first, endpoints[i].second,
                             endpoints[i + 1].first, endpoints[i + 1].second);
  }

  return ChainBuilder{*this, last_spec};
}

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_GRAPH_BUILDER_HPP_
