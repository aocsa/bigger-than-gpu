#include "quest/taskflow/graph.hpp"

#include <format>
#include <utility>

#include "quest/taskflow/meta_pipeline.hpp"

namespace quest::taskflow {

// =============================================================================
// Graph Construction
// =============================================================================

Graph::Graph(std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : memory_resource_manager_(std::move(memory_manager)) {
  operator_lookup_table_[kGraphEntryPointId] = nullptr;
}

std::size_t Graph::AddNode(Operator* node_operator) {
  if (node_operator == nullptr) {
    return kGraphEntryPointId;
  }

  auto operator_id = static_cast<std::int32_t>(node_operator->id());

  if (operator_lookup_table_.contains(operator_id)) {
    return node_operator->id();
  }

  operator_lookup_table_[operator_id] = node_operator;
  registered_operators_.push_back(node_operator);
  node_operator->set_graph(shared_from_this());

  return node_operator->id();
}

void Graph::AddEdge(Operator* source_operator, Operator* target_operator,
                    const std::string& source_port_name, const std::string& target_port_name,
                    const EdgeConfig& edge_config, const std::string& edge_name) {
  AddNode(source_operator);
  AddNode(target_operator);

  auto source_id = static_cast<std::int32_t>(source_operator->id());
  auto target_id = static_cast<std::int32_t>(target_operator->id());

  Edge new_edge{.source_operator_id = source_id,
                .target_operator_id = target_id,
                .source_port_name = source_port_name,
                .target_port_name = target_port_name};

  outgoing_edges_[source_id].insert(new_edge);
  incoming_edges_[target_id].insert(new_edge);
  target_operator->set_parent(source_id);

  // Keyed by endpoint identity; see GetEdgeConfig() for why this lives in a
  // side map instead of on Edge itself.
  edge_configs_[{source_id, target_id}] = edge_config;

  // Holders materialize in Prepare()/Materialize().
  edge_specs_.push_back(EdgeSpec{.source = source_operator,
                                 .target = target_operator,
                                 .source_port_name = source_port_name,
                                 .target_port_name = target_port_name,
                                 .config = edge_config,
                                 .name = edge_name,
                                 .synthesized = false});

  if (!source_operator->has_parent()) {
    Edge sentinel_edge{.source_operator_id = kGraphEntryPointId,
                       .target_operator_id = source_id,
                       .source_port_name = "",
                       .target_port_name = target_port_name};
    outgoing_edges_[kGraphEntryPointId].insert(sentinel_edge);
  }
}

void Graph::AddTuningPolicy(EdgeTuningPolicy policy) {
  tuning_policies_.push_back(std::move(policy));
}

EdgeInfo Graph::MakeEdgeInfo(const EdgeSpec& spec) const {
  EdgeInfo info;
  info.ref = EdgeRef{.source_id = static_cast<std::int32_t>(spec.source->id()),
                     .target_id = static_cast<std::int32_t>(spec.target->id()),
                     .target_port = spec.target_port_name};
  info.name = spec.name;
  info.synthesized = spec.synthesized;
  info.config = spec.config;
  info.source_type = spec.source->type();
  info.target_type = spec.target->type();
  return info;
}

std::vector<EdgeInfo> Graph::Edges() const {
  std::vector<EdgeInfo> infos;
  infos.reserve(edge_specs_.size());
  for (const auto& spec : edge_specs_) {
    infos.push_back(MakeEdgeInfo(spec));
  }
  return infos;
}

Graph::EdgeSpec* Graph::FindEdgeSpec(const EdgeRef& ref) {
  const std::string default_port = std::to_string(ref.target_id);
  for (auto& spec : edge_specs_) {
    const auto source_id = static_cast<std::int32_t>(spec.source->id());
    const auto target_id = static_cast<std::int32_t>(spec.target->id());
    if (source_id != ref.source_id || target_id != ref.target_id) {
      continue;
    }
    if (ref.target_port.empty()) {
      if (spec.target_port_name.empty() || spec.target_port_name == default_port) {
        return &spec;
      }
    } else if (spec.target_port_name == ref.target_port) {
      return &spec;
    }
  }
  return nullptr;
}

Graph::EdgeSpec* Graph::FindEdgeSpecByName(std::string_view name) {
  for (auto& spec : edge_specs_) {
    if (spec.name == name) {
      return &spec;
    }
  }
  return nullptr;
}

Result<EdgeTuner> Graph::Tune(const EdgeRef& ref) {
  auto* spec = FindEdgeSpec(ref);
  if (spec == nullptr) {
    return std::unexpected(
        Error::Invalid(std::format("Graph::Tune: no edge {}:{}:{}", ref.source_id, ref.target_id,
                                    ref.target_port))
            .error());
  }
  return EdgeTuner{this, spec};
}

Result<EdgeTuner> Graph::Tune(std::string_view edge_name) {
  auto* spec = FindEdgeSpecByName(edge_name);
  if (spec == nullptr) {
    return std::unexpected(
        Error::Invalid(std::format("Graph::Tune: no edge named '{}'", edge_name)).error());
  }
  return EdgeTuner{this, spec};
}

void Graph::SynthesizeStructure() {
  // Phase 6 plugs partition/merge injection here. Empty so Prepare()'s
  // order (synthesize → resolve → materialize) is already correct.
  // Synthesized edges should set EdgeSpec::synthesized and use §6.1 naming:
  // "<consumer_name>_<consumer_id>.<port>~synth<k>".
}

std::vector<EdgeConfig> Graph::ResolveConfigs(const QueryStats& stats, const HardwareInfo& hw) {
  // Author + contract already applied at GraphBuilder::Build(); Tune() may have
  // refined Built specs. Policies refine in registration order from that base;
  // last write wins per field (§5). Resolved configs are returned — Built
  // edge_specs_[].config is NOT mutated so Re-Prepare stays independent.
  std::vector<EdgeConfig> resolved;
  resolved.reserve(edge_specs_.size());

  for (auto& spec : edge_specs_) {
    EdgeConfig running = spec.config;
    for (const auto& policy : tuning_policies_) {
      EdgeInfo snapshot = MakeEdgeInfo(spec);
      snapshot.config = running;
      running = policy(snapshot, stats, hw);
    }
    resolved.push_back(running);

    const auto source_id = static_cast<std::int32_t>(spec.source->id());
    const auto target_id = static_cast<std::int32_t>(spec.target->id());
    edge_configs_[{source_id, target_id}] = running;
  }
  return resolved;
}

void Graph::RestoreAuthorEdgeConfigs() {
  for (const auto& spec : edge_specs_) {
    const auto source_id = static_cast<std::int32_t>(spec.source->id());
    const auto target_id = static_cast<std::int32_t>(spec.target->id());
    edge_configs_[{source_id, target_id}] = spec.config;
  }
}

std::vector<PreparedEdge> Graph::MaterializeHolders(
    const std::vector<EdgeConfig>& resolved_configs) {
  std::vector<PreparedEdge> prepared_edges;
  prepared_edges.reserve(edge_specs_.size());

  for (std::size_t i = 0; i < edge_specs_.size(); ++i) {
    const auto& spec = edge_specs_[i];
    const EdgeConfig& cfg = resolved_configs[i];
    auto batch_holders = CreateBatchHolders(cfg);

    if (cfg.num_partitions > 1) {
      for (std::size_t partition_index = 0; partition_index < batch_holders.size();
           ++partition_index) {
        auto output_port_name = std::format("output_{}", partition_index);
        auto input_port_name = std::format("input_{}", partition_index);

        spec.source->output().RegisterBatchHolder(output_port_name, batch_holders[partition_index]);
        spec.target->input().RegisterBatchHolder(input_port_name, batch_holders[partition_index]);
      }
    } else {
      spec.source->output().RegisterBatchHolder(spec.source_port_name, batch_holders[0]);
      spec.target->input().RegisterBatchHolder(spec.target_port_name, batch_holders[0]);
    }

    EdgeInfo info = MakeEdgeInfo(spec);
    info.config = cfg;
    prepared_edges.push_back(
        PreparedEdge{.info = std::move(info), .holders = std::move(batch_holders)});
  }

  return prepared_edges;
}

void Graph::Materialize() {
  // Materialize with Built (author) configs — no policy resolution.
  std::vector<EdgeConfig> configs;
  configs.reserve(edge_specs_.size());
  for (const auto& spec : edge_specs_) {
    configs.push_back(spec.config);
  }
  (void)MaterializeHolders(configs);
}

Result<PreparedQuery> Graph::Prepare(const QueryStats& stats, const HardwareInfo& hw) {
  // §5 / §7: synthesize → resolve (policy chain) → materialize → pipelines.
  SynthesizeStructure();
  auto resolved = ResolveConfigs(stats, hw);
  auto prepared_edges = MaterializeHolders(resolved);

  auto build_result = MetaPipeline::Build(*this);
  // Restore Built edge_configs_ so the Graph remains reusable for another Prepare.
  RestoreAuthorEdgeConfigs();

  if (!build_result.has_value()) {
    return std::unexpected(build_result.error());
  }

  return PreparedQuery{std::move(prepared_edges), std::move(build_result.value())};
}

std::vector<std::shared_ptr<cache::IBatchHolder>> Graph::CreateBatchHolders(
    const EdgeConfig& edge_config) {
  std::vector<std::shared_ptr<cache::IBatchHolder>> holders;
  holders.reserve(edge_config.num_partitions);

  // Never → auto_spill=false; Eligible/First → auto_spill=true (phase 5 expands).
  cache::BatchHolderConfig holder_config{
      .flow_control_batches_threshold = edge_config.flow_control_batches_threshold,
      .flow_control_bytes_threshold = edge_config.flow_control_bytes_threshold,
      .auto_spill = (edge_config.spill_policy != SpillPolicy::Never),
      .concat_bytes_threshold = edge_config.concat_bytes_threshold};

  for (std::size_t partition = 0; partition < edge_config.num_partitions; ++partition) {
    holders.push_back(cache::MakeBatchHolder(GenerateNextBatchHolderId(), edge_config.type,
                                             holder_config, memory_resource_manager_));
  }

  return holders;
}

// =============================================================================
// EdgeTuner
// =============================================================================

void EdgeTuner::SyncSideMap() noexcept {
  const auto source_id = static_cast<std::int32_t>(spec_->source->id());
  const auto target_id = static_cast<std::int32_t>(spec_->target->id());
  graph_->edge_configs_[{source_id, target_id}] = spec_->config;
}

EdgeTuner& EdgeTuner::Full() noexcept {
  spec_->config.barrier = BarrierType::Full;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::Streaming() noexcept {
  spec_->config.barrier = BarrierType::Pipeline;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::Concatenating() noexcept {
  spec_->config.type = cache::BatchHolderType::kConcatenating;
  spec_->config.barrier = BarrierType::Full;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::Partitioned(std::size_t partition_count) noexcept {
  spec_->config.num_partitions = partition_count;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::PartitionedUpTo(std::size_t max_partitions) noexcept {
  spec_->config.max_partitions = max_partitions;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::FlowControl(std::size_t max_batches, std::size_t max_bytes) noexcept {
  spec_->config.flow_control_batches_threshold = max_batches;
  spec_->config.flow_control_bytes_threshold = max_bytes;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::Spill(SpillPolicy policy) noexcept {
  spec_->config.spill_policy = policy;
  SyncSideMap();
  return *this;
}

EdgeTuner& EdgeTuner::Config(EdgeConfig config) noexcept {
  spec_->config = std::move(config);
  SyncSideMap();
  return *this;
}

// =============================================================================
// Graph Execution
// =============================================================================

Status Graph::Execute() {
  // Phase 2.5: Execute is sugar for Prepare({}, {}).Execute().
  auto prepared = Prepare(QueryStats{}, HardwareInfo{});
  if (!prepared.has_value()) {
    return std::unexpected(prepared.error());
  }
  return prepared->Execute();
}

// =============================================================================
// Graph Queries
// =============================================================================

Operator* Graph::GetNode(std::size_t operator_id) const {
  auto iterator = operator_lookup_table_.find(static_cast<std::int32_t>(operator_id));
  return iterator != operator_lookup_table_.end() ? iterator->second : nullptr;
}

Operator& Graph::GetLastOperator() {
  if (registered_operators_.empty()) {
    throw std::runtime_error("Graph has no operators");
  }
  return *registered_operators_.back();
}

const Operator& Graph::GetLastOperator() const {
  if (registered_operators_.empty()) {
    throw std::runtime_error("Graph has no operators");
  }
  return *registered_operators_.back();
}

std::set<Edge> Graph::GetOutgoingEdges(Operator* from_operator) const {
  if (from_operator == nullptr) {
    return {};
  }
  return GetOutgoingEdges(static_cast<std::int32_t>(from_operator->id()));
}

std::set<Edge> Graph::GetOutgoingEdges(std::int32_t operator_id) const {
  auto iterator = outgoing_edges_.find(operator_id);
  return iterator != outgoing_edges_.end() ? iterator->second : std::set<Edge>{};
}

std::set<Edge> Graph::GetIncomingEdges(Operator* to_operator) const {
  if (to_operator == nullptr) {
    return {};
  }
  return GetIncomingEdges(static_cast<std::int32_t>(to_operator->id()));
}

std::set<Edge> Graph::GetIncomingEdges(std::int32_t operator_id) const {
  auto iterator = incoming_edges_.find(operator_id);
  return iterator != incoming_edges_.end() ? iterator->second : std::set<Edge>{};
}

EdgeConfig Graph::GetEdgeConfig(std::int32_t source_operator_id,
                                std::int32_t target_operator_id) const {
  auto iterator = edge_configs_.find({source_operator_id, target_operator_id});
  return iterator != edge_configs_.end() ? iterator->second : EdgeConfig{};
}

std::vector<Operator*> Graph::GetTerminalOperators() const {
  std::vector<Operator*> terminals;
  for (auto* current_operator : registered_operators_) {
    if (GetOutgoingEdges(current_operator).empty()) {
      terminals.push_back(current_operator);
    }
  }
  return terminals;
}

}  // namespace quest::taskflow
