// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/graph_builder.hpp"

#include <algorithm>
#include <format>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace quest::taskflow {

// =============================================================================
// GraphBuilder
// =============================================================================

GraphBuilder::GraphBuilder(
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : memory_manager_(std::move(memory_manager)) {}

GraphBuilder::EdgeSpec& GraphBuilder::AddEdgeSpec(Operator* src, std::string src_port,
                                                   Operator* tgt, std::string tgt_port) {
  edge_specs_.push_back(EdgeSpec{.source = src,
                                 .source_port = std::move(src_port),
                                 .target = tgt,
                                 .target_port = std::move(tgt_port)});
  return edge_specs_.back();
}

EdgeBuilder GraphBuilder::Edge(Operator& src, Operator& dst) {
  auto& spec = AddEdgeSpec(&src, "", &dst, "");
  return EdgeBuilder{*this, &spec};
}

EdgeBuilder GraphBuilder::Edge(Operator& src, OperatorPortPair dst) {
  auto& spec = AddEdgeSpec(&src, "", dst.first, std::move(dst.second));
  return EdgeBuilder{*this, &spec};
}

EdgeBuilder GraphBuilder::Edge(OperatorPortPair src, Operator& dst) {
  auto& spec = AddEdgeSpec(src.first, std::move(src.second), &dst, "");
  return EdgeBuilder{*this, &spec};
}

EdgeBuilder GraphBuilder::Edge(OperatorPortPair src, OperatorPortPair dst) {
  auto& spec =
      AddEdgeSpec(src.first, std::move(src.second), dst.first, std::move(dst.second));
  return EdgeBuilder{*this, &spec};
}

// ---------------------------------------------------------------------------
// Build() — validation + graph construction
// ---------------------------------------------------------------------------

namespace {

/// @brief Config resolution (§5): contract as base, author's explicit fields on top.
///
/// Priority (high→low): Config() override > explicit fluent calls > InputEdgeContract > defaults.
EdgeConfig ResolveEdgeConfig(const GraphBuilder::EdgeSpec& spec) {
  if (spec.config_replaced) {
    return spec.config;
  }

  EdgeConfig contract;
  if (spec.target != nullptr) {
    contract = spec.target->InputEdgeContract(spec.target_port);
  }

  EdgeConfig result = contract;

  // Layer 3 (author) overrides layer 4 (contract).
  if (spec.explicit_barrier) {
    result.barrier = spec.config.barrier;
  }
  if (spec.explicit_type) {
    // Only holder-type verbs (Concatenating/ConcatenatingUpTo/NonWaiting) set
    // explicit_type; Partitioned() deliberately does NOT — so a partition
    // count never clobbers a contract's holder type (review P3).
    result.type = spec.config.type;
    result.concat_bytes_threshold = spec.config.concat_bytes_threshold;
    // Concatenating implies Full unless author explicitly overrode the barrier.
    if (spec.config.type == cache::BatchHolderType::kConcatenating &&
        !spec.explicit_barrier) {
      result.barrier = BarrierType::Full;
    }
  }
  if (spec.explicit_flow_control) {
    result.flow_control_batches_threshold = spec.config.flow_control_batches_threshold;
    result.flow_control_bytes_threshold = spec.config.flow_control_bytes_threshold;
  }
  if (spec.explicit_spill) {
    result.spill_policy = spec.config.spill_policy;
  }
  if (spec.explicit_partitions) {
    if (spec.config.num_partitions > 1) {
      result.num_partitions = spec.config.num_partitions;
    }
    if (spec.config.max_partitions > 0) {
      result.max_partitions = spec.config.max_partitions;
    }
  }

  return result;
}

/// @brief Check: NonWaiting edges reject knobs that contradict their contract.
Status CheckNonWaiting(const GraphBuilder::EdgeSpec& spec) {
  if (spec.config_replaced ||
      spec.config.type != cache::BatchHolderType::kNonWaiting) {
    return Status::OK();
  }
  if (spec.explicit_flow_control) {
    return Status::Invalid(
        "GraphBuilder: .NonWaiting() edges cannot use .FlowControl() — a "
        "pre-filled single-shot handoff has no producer to throttle.");
  }
  if (spec.explicit_barrier && spec.config.barrier == BarrierType::Full) {
    return Status::Invalid(
        "GraphBuilder: .NonWaiting() edges cannot be .Full() — empty means "
        "drained; there is no producer pipeline to wait on.");
  }
  return Status::OK();
}

/// @brief Check (e): Partial() is not yet supported.
Status CheckPartial(const GraphBuilder::EdgeSpec& spec) {
  if (spec.partial_requested) {
    return Status::Invalid(
        "GraphBuilder: .Partial() is reserved for phase 6 and is not yet supported. "
        "Use .Full() or .Streaming() instead.");
  }
  return Status::OK();
}

/// @brief Check (c): explicit barrier contradiction with the consumer's InputEdgeContract.
Status CheckContractConflict(const GraphBuilder::EdgeSpec& spec) {
  if (spec.config_replaced || !spec.explicit_barrier || spec.target == nullptr) {
    return Status::OK();
  }

  EdgeConfig contract = spec.target->InputEdgeContract(spec.target_port);

  // A conflict exists only if the contract declares a non-default barrier
  // and the author's explicit barrier differs from it.
  if (contract.barrier != BarrierType::Pipeline &&
      spec.config.barrier != contract.barrier) {
    return Status::Invalid(std::format(
        "GraphBuilder: explicit barrier contradicts the input contract of operator '{}' "
        "(port '{}'). The contract requires {}; author set {}. "
        "Use .Config() to override the contract.",
        spec.target->expression(), spec.target_port,
        to_string(contract.barrier), to_string(spec.config.barrier)));
  }
  return Status::OK();
}

/// @brief Check (a): named ports on the target operator.
Status CheckDeclaredPorts(const GraphBuilder::EdgeSpec& spec) {
  if (spec.target == nullptr) { return Status::OK(); }

  auto declared = spec.target->DeclaredInputPorts();
  if (declared.empty()) { return Status::OK(); }  // legacy behavior: any name allowed

  const std::string_view port_name = spec.target_port;
  bool found = std::ranges::any_of(declared,
      [&port_name](std::string_view decl) { return decl == port_name; });

  if (!found) {
    return Status::Invalid(std::format(
        "GraphBuilder: port '{}' is not declared on operator '{}'. "
        "Declared ports: [{}]. Check for typos.",
        port_name, spec.target->expression(),
        [&]() {
          std::string list;
          for (std::string_view decl : declared) {
            if (!list.empty()) { list += ", "; }
            list += "'";
            list += decl;
            list += "'";
          }
          return list;
        }()));
  }
  return Status::OK();
}

/// @brief Check (b): acyclicity via DFS coloring.
Status CheckAcyclic(const std::deque<GraphBuilder::EdgeSpec>& specs) {
  if (specs.empty()) { return Status::OK(); }

  // Build adjacency list.
  std::map<Operator*, std::vector<Operator*>> adj;
  for (const auto& spec : specs) {
    if (spec.source != nullptr && spec.target != nullptr) {
      adj[spec.source].push_back(spec.target);
      adj.emplace(spec.target, std::vector<Operator*>{});  // ensure target exists
    }
  }

  enum class Color : std::uint8_t { White, Gray, Black };
  std::map<Operator*, Color> color;
  for (const auto& [oper, ignored] : adj) { color[oper] = Color::White; }

  // DFS closure using std::function for recursion.
  std::function<Status(Operator*)> dfs = [&](Operator* node) -> Status {
    color[node] = Color::Gray;
    for (Operator* next : adj[node]) {
      if (color[next] == Color::Gray) {
        return Status::Invalid(std::format(
            "GraphBuilder: cycle detected involving operator '{}'. "
            "Execution graphs must be acyclic.",
            next->expression()));
      }
      if (color[next] == Color::White) {
        auto status = dfs(next);
        if (!status.ok()) { return status; }
      }
    }
    color[node] = Color::Black;
    return Status::OK();
  };

  for (const auto& [oper, ignored] : adj) {
    if (color[oper] == Color::White) {
      auto status = dfs(oper);
      if (!status.ok()) { return status; }
    }
  }
  return Status::OK();
}

}  // namespace

Result<std::shared_ptr<Graph>> GraphBuilder::Build() && {
  // (d) Duplicate edge names.
  std::set<std::string> seen_names;
  for (const auto& spec : edge_specs_) {
    if (spec.name.empty()) { continue; }
    if (!seen_names.insert(spec.name).second) {
      return std::unexpected(Status::Invalid(std::format(
          "GraphBuilder: duplicate edge name '{}'. Edge names must be unique.", spec.name)).error());
    }
  }

  for (const auto& spec : edge_specs_) {
    if (auto status = CheckPartial(spec); !status.ok()) { return std::unexpected(status.error()); }
    if (auto status = CheckDeclaredPorts(spec); !status.ok()) { return std::unexpected(status.error()); }
    if (auto status = CheckContractConflict(spec); !status.ok()) { return std::unexpected(status.error()); }
    if (auto status = CheckNonWaiting(spec); !status.ok()) { return std::unexpected(status.error()); }
  }

  // (b) Acyclicity.
  if (auto status = CheckAcyclic(edge_specs_); !status.ok()) { return std::unexpected(status.error()); }

  auto graph = std::make_shared<Graph>(std::move(memory_manager_));

  for (const auto& spec : edge_specs_) {
    if (spec.source == nullptr || spec.target == nullptr) { continue; }

    EdgeConfig resolved = ResolveEdgeConfig(spec);

    // Default port names resolve to operator-id strings (matches old DSL convention).
    std::string src_port =
        spec.source_port.empty() ? std::to_string(spec.source->id()) : spec.source_port;
    std::string tgt_port =
        spec.target_port.empty() ? std::to_string(spec.target->id()) : spec.target_port;

    graph->AddEdge(spec.source, spec.target, src_port, tgt_port, resolved, spec.name);
  }

  return graph;
}

// =============================================================================
// EdgeBuilder
// =============================================================================

EdgeBuilder& EdgeBuilder::Full() noexcept {
  spec_->config.barrier = BarrierType::Full;
  spec_->explicit_barrier = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::Streaming() noexcept {
  spec_->config.barrier = BarrierType::Pipeline;
  spec_->explicit_barrier = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::Partial() noexcept {
  spec_->partial_requested = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::Concatenating() noexcept {
  spec_->config.type = cache::BatchHolderType::kConcatenating;
  spec_->config.barrier = BarrierType::Full;  // Concatenating implies Full
  spec_->explicit_type = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::ConcatenatingUpTo(std::size_t bytes) noexcept {
  spec_->config.type = cache::BatchHolderType::kConcatenating;
  spec_->config.barrier = BarrierType::Full;  // implies Full, like Concatenating
  spec_->config.concat_bytes_threshold = bytes;
  spec_->explicit_type = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::NonWaiting() noexcept {
  spec_->config.type = cache::BatchHolderType::kNonWaiting;
  spec_->explicit_type = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::Partitioned(std::size_t partition_count) noexcept {
  spec_->config.num_partitions = partition_count;
  spec_->explicit_partitions = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::PartitionedUpTo(std::size_t max_partitions) noexcept {
  spec_->config.max_partitions = max_partitions;
  spec_->explicit_partitions = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::FlowControl(std::size_t max_batches,
                                       std::size_t max_bytes) noexcept {
  spec_->config.flow_control_batches_threshold = max_batches;
  spec_->config.flow_control_bytes_threshold = max_bytes;
  spec_->explicit_flow_control = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::Spill(SpillPolicy policy) noexcept {
  spec_->config.spill_policy = policy;
  spec_->explicit_spill = true;
  return *this;
}

EdgeBuilder& EdgeBuilder::Named(std::string edge_name) {
  spec_->name = std::move(edge_name);
  return *this;
}

EdgeBuilder& EdgeBuilder::Config(EdgeConfig config) noexcept {
  spec_->config = config;
  spec_->config_replaced = true;
  return *this;
}

EdgeBuilder EdgeBuilder::ThenTo(Operator& next) {
  auto& new_spec = builder_->AddEdgeSpec(spec_->target, "", &next, "");
  return EdgeBuilder{*builder_, &new_spec};
}

EdgeBuilder EdgeBuilder::ThenTo(OperatorPortPair next) {
  auto& new_spec =
      builder_->AddEdgeSpec(spec_->target, "", next.first, std::move(next.second));
  return EdgeBuilder{*builder_, &new_spec};
}

}  // namespace quest::taskflow
