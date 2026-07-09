#include "quest/taskflow/graph.hpp"

#include <algorithm>
#include <format>
#include <mutex>
#include <print>

namespace quest::taskflow {

// Default limit value used when optimizing scan-with-limit queries.
// This is a placeholder; a full implementation would parse the actual limit.
constexpr std::int64_t kDefaultOptimizedLimitRows = 1000;

// =============================================================================
// Graph Construction
// =============================================================================

Graph::Graph(std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : memory_resource_manager_(std::move(memory_manager)) {
  // Initialize the sentinel entry point node.
  // This virtual node serves as the parent for all root operators (sources).
  operator_lookup_table_[kGraphEntryPointId] = nullptr;
}

KPair& Graph::operator+=(KPair edge_pair) {
  // Determine port names for the connection.
  // If no custom port is specified, use the operator's ID as the port name.
  std::string source_port = edge_pair.has_custom_source()
                                ? edge_pair.source_port()
                                : std::to_string(edge_pair.source()->id());

  std::string target_port = edge_pair.has_custom_destination()
                                ? edge_pair.destination_port()
                                : std::to_string(edge_pair.destination()->id());

  // Create the edge with the resolved port names.
  AddEdge(edge_pair.source(), edge_pair.destination(), source_port, target_port,
          edge_pair.config());

  // Store the edge for potential fluent chaining.
  most_recent_edge_ = std::make_unique<KPair>(std::move(edge_pair));
  return *most_recent_edge_;
}

std::size_t Graph::AddNode(Operator* node_operator) {
  // Handle null operator by returning sentinel ID.
  if (node_operator == nullptr) {
    return kGraphEntryPointId;
  }

  auto operator_id = static_cast<std::int32_t>(node_operator->id());

  // Skip if operator is already registered.
  if (operator_lookup_table_.contains(operator_id)) {
    return node_operator->id();
  }

  // Register the operator in both data structures.
  operator_lookup_table_[operator_id] = node_operator;
  registered_operators_.push_back(node_operator);

  // Establish bidirectional reference between operator and graph.
  node_operator->set_graph(shared_from_this());

  return node_operator->id();
}

void Graph::AddEdge(Operator* source_operator, Operator* target_operator,
                    const std::string& source_port_name, const std::string& target_port_name,
                    const EdgeConfig& edge_config) {
  // Ensure both operators are registered in the graph.
  AddNode(source_operator);
  AddNode(target_operator);

  auto source_id = static_cast<std::int32_t>(source_operator->id());
  auto target_id = static_cast<std::int32_t>(target_operator->id());

  // Create the edge structure with resolved identifiers.
  Edge new_edge{.source_operator_id = source_id,
                .target_operator_id = target_id,
                .source_port_name = source_port_name,
                .target_port_name = target_port_name};

  // Add to both adjacency lists for bidirectional traversal.
  outgoing_edges_[source_id].insert(new_edge);
  incoming_edges_[target_id].insert(new_edge);

  // Establish the parent-child dependency relationship.
  target_operator->set_parent(source_id);

  // Remember the configuration this edge was created with, keyed by
  // endpoint identity (see GetEdgeConfig() for why this lives in a side
  // map instead of on Edge itself).
  edge_configs_[{source_id, target_id}] = edge_config;

  // Create the batch holders that will carry data between operators.
  auto batch_holders = CreateBatchHolders(edge_config);

  // Connect batch holders to operator ports based on partitioning mode.
  if (edge_config.num_partitions > 1) {
    // Partitioned mode: each partition gets its own batch holder.
    // This enables parallel processing patterns (e.g., FOR_EACH).
    for (std::size_t partition_index = 0; partition_index < batch_holders.size();
         ++partition_index) {
      auto output_port_name = std::format("output_{}", partition_index);
      auto input_port_name = std::format("input_{}", partition_index);

      source_operator->output().RegisterBatchHolder(output_port_name,
                                                    batch_holders[partition_index]);
      target_operator->input().RegisterBatchHolder(input_port_name, batch_holders[partition_index]);
    }
  } else {
    // Simple mode: single batch holder connecting the ports directly.
    source_operator->output().RegisterBatchHolder(source_port_name, batch_holders[0]);
    target_operator->input().RegisterBatchHolder(target_port_name, batch_holders[0]);
  }

  // If source has no parent, connect it to the sentinel entry point.
  // This marks it as a root operator in the graph.
  if (!source_operator->has_parent()) {
    Edge sentinel_edge{.source_operator_id = kGraphEntryPointId,
                       .target_operator_id = source_id,
                       .source_port_name = "",
                       .target_port_name = target_port_name};
    outgoing_edges_[kGraphEntryPointId].insert(sentinel_edge);
  }
}

std::vector<std::shared_ptr<cache::IBatchHolder>> Graph::CreateBatchHolders(
    const EdgeConfig& edge_config) {
  std::vector<std::shared_ptr<cache::IBatchHolder>> holders;
  holders.reserve(edge_config.num_partitions);

  // Configure flow control thresholds for backpressure.
  cache::BatchHolderConfig holder_config{
      .flow_control_batches_threshold = edge_config.flow_control_batches_threshold,
      .flow_control_bytes_threshold = edge_config.flow_control_bytes_threshold};

  // Create the requested number of batch holders.
  for (std::size_t partition = 0; partition < edge_config.num_partitions; ++partition) {
    holders.push_back(cache::MakeBatchHolder(GenerateNextBatchHolderId(), edge_config.type,
                                             holder_config, memory_resource_manager_));
  }

  return holders;
}

// =============================================================================
// Graph Execution
// =============================================================================

void Graph::FinalizeWorkflow() {
  // Iterate through all registered operators to find terminal nodes.
  for (auto& [operator_id, current_operator] : operator_lookup_table_) {
    // Skip the sentinel node and check for missing outgoing edges.
    if (current_operator != nullptr && GetOutgoingEdges(current_operator).empty()) {
      // Mark this as a terminal node by adding a sentinel target edge.
      auto current_id = static_cast<std::int32_t>(current_operator->id());
      Edge terminal_edge{.source_operator_id = current_id,
                         .target_operator_id = -1,  // Sentinel indicates terminal
                         .source_port_name = std::to_string(current_operator->id()),
                         .target_port_name = ""};
      outgoing_edges_[current_id].insert(terminal_edge);
    }
  }
}

Status Graph::Execute() {
  // Prepare the graph by marking terminal operators.
  FinalizeWorkflow();

  // Thread pool for parallel operator execution.
  std::vector<ExecutionThread> execution_threads;

  // Track which edges have been processed to avoid duplicate execution.
  std::set<std::pair<std::size_t, std::size_t>> processed_edges;

  // Work queue containing operator IDs ready for execution.
  std::deque<std::size_t> ready_operator_queue;

  // Mutex protecting the work queue for thread-safe access.
  std::mutex queue_mutex;

  // Seed the queue with root operators (connected to sentinel).
  for (const auto& edge : GetOutgoingEdges(kGraphEntryPointId)) {
    ready_operator_queue.push_back(edge.target_operator_id);
  }

  // Process operators until the queue is exhausted.
  while (!ready_operator_queue.empty()) {
    // Dequeue the next operator to process.
    std::size_t current_operator_id;
    {
      std::lock_guard<std::mutex> lock(queue_mutex);
      if (ready_operator_queue.empty()) {
        break;
      }
      current_operator_id = ready_operator_queue.front();
      ready_operator_queue.pop_front();
    }

    auto* current_operator = GetNode(current_operator_id);
    if (current_operator == nullptr) {
      continue;
    }

    // Check if all dependencies (incoming edges) are satisfied.
    auto dependency_edges = GetIncomingEdges(current_operator);
    bool all_dependencies_satisfied =
        dependency_edges.empty() ||
        std::all_of(
            dependency_edges.begin(), dependency_edges.end(), [&processed_edges](const Edge& dep) {
              return processed_edges.contains({dep.source_operator_id, dep.target_operator_id});
            });

    if (!all_dependencies_satisfied) {
      // Re-queue this operator to try again after dependencies complete.
      std::lock_guard<std::mutex> lock(queue_mutex);
      ready_operator_queue.push_back(current_operator_id);
      continue;
    }

    // Process all outgoing edges from this operator.
    for (const auto& edge : GetOutgoingEdges(current_operator)) {
      auto edge_key =
          std::make_pair(current_operator_id, static_cast<std::size_t>(edge.target_operator_id));

      // Skip already processed edges.
      if (processed_edges.contains(edge_key)) {
        continue;
      }

      processed_edges.insert(edge_key);

      // Queue downstream operators for execution.
      if (edge.target_operator_id >= 0) {
        std::lock_guard<std::mutex> lock(queue_mutex);
        ready_operator_queue.push_back(edge.target_operator_id);
      }

      // Launch the operator in a dedicated thread.
      execution_threads.emplace_back(
          [current_operator, current_operator_id, target = edge.target_operator_id]() {
            auto execution_status = current_operator->run();

            if (execution_status == ExecutionStatus::Proceed) {
              // Signal completion to downstream operators.
              current_operator->output().Finish();
              current_operator->mark_done();
            } else if (target >= 0) {
              std::println(stderr, "ERROR: Operator {} did not finish successfully",
                           current_operator_id);
            }
          });
    }
  }

  // Wait for all operator threads to complete.
  for (auto& thread : execution_threads) {
    if (thread.joinable()) {
      thread.join();
    }
  }

  return Status::OK();
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

std::pair<bool, std::uint64_t> Graph::EstimateInputRowCount(std::int32_t operator_id) const {
  auto* target_operator = GetNode(operator_id);
  if (target_operator == nullptr) {
    return {false, 0};
  }

  // If all inputs are complete, return the actual count.
  if (target_operator->InputAllFinished()) {
    return {true, target_operator->TotalInputRowsAdded()};
  }

  // Try to estimate from upstream operators.
  auto upstream_edges = GetIncomingEdges(operator_id);
  if (upstream_edges.size() == 1) {
    auto* source_operator = GetNode(upstream_edges.begin()->source_operator_id);
    if (source_operator != nullptr) {
      return source_operator->GetEstimatedOutputNumRows();
    }
  }

  return {false, 0};
}

std::pair<bool, std::uint64_t> Graph::EstimateInputRowCountForPort(
    std::int32_t operator_id, const std::string& port_name) const {
  auto* target_operator = GetNode(operator_id);
  if (target_operator == nullptr) {
    return {false, 0};
  }

  // If the specific port is complete, return the actual count.
  if (target_operator->InputCacheFinished(port_name)) {
    return {true, target_operator->InputCacheNumRowsAdded(port_name)};
  }

  // Find the upstream operator connected to this specific port.
  auto upstream_edges = GetIncomingEdges(operator_id);
  for (const auto& edge : upstream_edges) {
    if (edge.target_port_name == port_name) {
      auto* source_operator = GetNode(edge.source_operator_id);
      if (source_operator != nullptr) {
        return source_operator->GetEstimatedOutputNumRows();
      }
    }
  }

  return {false, 0};
}

// =============================================================================
// Debug and Visualization
// =============================================================================

void Graph::PrintStructure() const {
  std::println("=== Execution Graph ===");
  std::println("Operators (id -> type):");

  // Print each operator with its type.
  for (const auto* current_operator : registered_operators_) {
    std::println("  {} -> {}", current_operator->id(), to_string(current_operator->type()));
  }

  std::println("\nEdges:");

  // Perform breadth-first traversal to print edges.
  std::set<std::pair<std::size_t, std::size_t>> visited_edges;
  std::deque<std::size_t> traversal_queue;

  // Start from root operators.
  for (const auto& edge : GetOutgoingEdges(kGraphEntryPointId)) {
    traversal_queue.push_back(edge.target_operator_id);
  }

  while (!traversal_queue.empty()) {
    auto current_id = traversal_queue.front();
    traversal_queue.pop_front();

    for (const auto& edge : GetOutgoingEdges(static_cast<std::int32_t>(current_id))) {
      auto edge_key = std::make_pair(current_id, static_cast<std::size_t>(edge.target_operator_id));

      if (visited_edges.contains(edge_key)) {
        continue;
      }

      visited_edges.insert(edge_key);
      std::println("  {} -> {}", current_id, edge.target_operator_id);

      if (edge.target_operator_id >= 0) {
        traversal_queue.push_back(edge.target_operator_id);
      }
    }
  }

  std::println("=======================");
}

void Graph::PrintBackwardsFrom(std::int32_t operator_id) const {
  std::println("=== Graph from operator {} (backwards) ===", operator_id);

  // Perform reverse breadth-first traversal.
  std::set<std::pair<std::size_t, std::size_t>> visited_edges;
  std::deque<std::size_t> traversal_queue;

  // Start from the specified operator's incoming edges.
  for (const auto& edge : GetIncomingEdges(operator_id)) {
    traversal_queue.push_back(edge.source_operator_id);
  }

  while (!traversal_queue.empty()) {
    auto current_id = traversal_queue.front();
    traversal_queue.pop_front();

    for (const auto& edge : GetIncomingEdges(static_cast<std::int32_t>(current_id))) {
      auto edge_key = std::make_pair(static_cast<std::size_t>(edge.source_operator_id), current_id);

      if (visited_edges.contains(edge_key)) {
        continue;
      }

      visited_edges.insert(edge_key);
      std::println("  {} <- {}", current_id, edge.source_operator_id);

      traversal_queue.push_back(edge.source_operator_id);
    }
  }

  std::println("=========================================");
}

// =============================================================================
// Query Optimization
// =============================================================================

void Graph::OptimizeScanWithLimit() {
  // This optimization only applies to simple three-operator queries.
  if (registered_operators_.size() != 3) {
    return;
  }

  // Identify operators by their type.
  Operator* scan_operator = nullptr;
  Operator* limit_operator = nullptr;
  Operator* output_operator = nullptr;

  for (auto* current_operator : registered_operators_) {
    switch (current_operator->type()) {
      case OperatorType::TableScan:
      case OperatorType::BindableTableScan:
        scan_operator = current_operator;
        break;
      case OperatorType::Limit:
        limit_operator = current_operator;
        break;
      case OperatorType::Output:
        output_operator = current_operator;
        break;
      default:
        // Unknown operator type breaks the pattern.
        return;
    }
  }

  // Apply optimization if the pattern matches.
  if (scan_operator != nullptr && limit_operator != nullptr && output_operator != nullptr) {
    // Push limit down to scan operator.
    // In a full implementation, this would parse the limit expression.
    scan_operator->set_limit(kDefaultOptimizedLimitRows);
  }
}

}  // namespace quest::taskflow
