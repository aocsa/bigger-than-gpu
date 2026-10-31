#ifndef QUEST_TASKFLOW_GRAPH_HPP_
#define QUEST_TASKFLOW_GRAPH_HPP_

/// @file graph.hpp
/// @brief Execution graph for the taskflow model.
///
/// The Graph class is the central orchestrator for query execution. It manages
/// a directed acyclic graph (DAG) of operators, where each operator represents
/// a computation unit (scan, filter, join, etc.) and edges represent data flow
/// between operators via batch holders.
///
/// Key responsibilities:
/// - Building the graph topology (edge specs; holders materialize in Prepare)
/// - Managing operator lifecycle and connections
/// - Preparing pipelines and resolved edge configurations
///
/// Lifecycle (Phase 2.5):
/// @code
///   auto graph = std::make_shared<Graph>();
///   // wire edges via GraphBuilder, then call Prepare
///   ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(stats, hw));
///   ASSERT_OK(query.Execute());
///   // or: graph->Execute();  // == Prepare({},{}).Execute()
/// @endcode

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/edge_ref.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/prepared_query.hpp"
#include "quest/taskflow/query_stats.hpp"

namespace quest::taskflow {

// Forward declaration — defined below Graph.
class EdgeTuner;

// =============================================================================
// Graph Edge Representation
// =============================================================================

/// @brief Represents a directed edge in the execution graph.
struct Edge {
  std::int32_t source_operator_id;
  std::int32_t target_operator_id;
  std::string source_port_name;
  std::string target_port_name;

  [[nodiscard]] auto operator<=>(const Edge& other) const noexcept {
    if (target_operator_id != other.target_operator_id) {
      return target_operator_id <=> other.target_operator_id;
    }
    return source_operator_id <=> other.source_operator_id;
  }

  [[nodiscard]] bool operator==(const Edge& other) const noexcept {
    return source_operator_id == other.source_operator_id &&
           target_operator_id == other.target_operator_id;
  }
};

// =============================================================================
// Tuning policy type
// =============================================================================

/// @brief Ordered policy applied per edge at Prepare() after synthesis.
using EdgeTuningPolicy = std::function<EdgeConfig(const EdgeInfo& edge, const QueryStats& stats,
                                                  const HardwareInfo& hw)>;

// =============================================================================
// Graph Class
// =============================================================================

/// @brief Execution graph managing operators and their data flow connections.
class Graph : public std::enable_shared_from_this<Graph> {
 public:
  /// @brief Edge recorded at wiring time; holders are created in Prepare().
  struct EdgeSpec {
    Operator* source{nullptr};
    Operator* target{nullptr};
    std::string source_port_name;
    std::string target_port_name;
    EdgeConfig config;
    std::string name;         ///< From GraphBuilder::Named(); empty if unnamed.
    bool synthesized{false};  ///< True for edges injected by SynthesizeStructure.
  };

  explicit Graph(std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  ~Graph() = default;

  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;
  Graph(Graph&&) = delete;
  Graph& operator=(Graph&&) = delete;

  // ===========================================================================
  // Graph Construction API
  // ===========================================================================

  std::size_t AddNode(Operator* node_operator);

  /// @brief Records a directed edge (structure only; holders in Prepare).
  ///
  /// @param edge_name Optional stable name from GraphBuilder::Named().
  void AddEdge(Operator* source_operator, Operator* target_operator,
               const std::string& source_port_name, const std::string& target_port_name,
               const EdgeConfig& edge_config, const std::string& edge_name = {});

  // ===========================================================================
  // Structural tuning (Built Graph only — before Prepare)
  // ===========================================================================

  void AddTuningPolicy(EdgeTuningPolicy policy);

  [[nodiscard]] Result<EdgeTuner> Tune(const EdgeRef& ref);
  [[nodiscard]] Result<EdgeTuner> Tune(std::string_view edge_name);

  /// @brief Pre-synthesis / Built edge introspection view.
  [[nodiscard]] std::vector<EdgeInfo> Edges() const;

  // ===========================================================================
  // Prepare / Materialize API
  // ===========================================================================

  [[nodiscard]] Result<PreparedQuery> Prepare(const QueryStats& stats, const HardwareInfo& hw);

  void Materialize();

  // ===========================================================================
  // Graph Execution API
  // ===========================================================================

  [[nodiscard]] Status Execute();

  // ===========================================================================
  // Graph Query API
  // ===========================================================================

  [[nodiscard]] Operator* GetNode(std::size_t operator_id) const;
  [[nodiscard]] Operator& GetLastOperator();
  [[nodiscard]] const Operator& GetLastOperator() const;
  [[nodiscard]] std::size_t GetNodeCount() const noexcept { return registered_operators_.size(); }

  [[nodiscard]] std::set<Edge> GetOutgoingEdges(Operator* from_operator) const;
  [[nodiscard]] std::set<Edge> GetOutgoingEdges(std::int32_t operator_id) const;
  [[nodiscard]] std::set<Edge> GetIncomingEdges(Operator* to_operator) const;
  [[nodiscard]] std::set<Edge> GetIncomingEdges(std::int32_t operator_id) const;

  [[nodiscard]] EdgeConfig GetEdgeConfig(std::int32_t source_operator_id,
                                         std::int32_t target_operator_id) const;

  [[nodiscard]] const std::vector<EdgeSpec>& edge_specs() const noexcept { return edge_specs_; }

  [[nodiscard]] std::vector<Operator*> GetTerminalOperators() const;

  [[nodiscard]] std::pair<bool, std::uint64_t> EstimateInputRowCount(
      std::int32_t operator_id) const;

  [[nodiscard]] std::pair<bool, std::uint64_t> EstimateInputRowCountForPort(
      std::int32_t operator_id, const std::string& port_name) const;

  void PrintStructure() const;
  void PrintBackwardsFrom(std::int32_t operator_id) const;

  [[nodiscard]] std::shared_ptr<memory::MemoryResourceManager> GetMemoryManager() const {
    return memory_resource_manager_;
  }

  void SetMemoryManager(std::shared_ptr<memory::MemoryResourceManager> manager) {
    memory_resource_manager_ = std::move(manager);
  }

 private:
  friend class EdgeTuner;

  static constexpr std::int32_t kGraphEntryPointId = -1;

  void SynthesizeStructure();

  /// @brief Apply policy chain; returns resolved configs without mutating Built specs.
  ///        Updates edge_configs_ temporarily for MetaPipeline::Build.
  [[nodiscard]] std::vector<EdgeConfig> ResolveConfigs(const QueryStats& stats,
                                                       const HardwareInfo& hw);

  /// @brief Materialize holders from resolved configs; restores edge_configs_ after.
  [[nodiscard]] std::vector<PreparedEdge> MaterializeHolders(
      const std::vector<EdgeConfig>& resolved_configs);

  [[nodiscard]] std::vector<std::shared_ptr<cache::IBatchHolder>> CreateBatchHolders(
      const EdgeConfig& edge_config);

  /// @brief Restore edge_configs_ side map from Built (author/Tune) specs.
  void RestoreAuthorEdgeConfigs();

  [[nodiscard]] EdgeInfo MakeEdgeInfo(const EdgeSpec& spec) const;

  [[nodiscard]] EdgeSpec* FindEdgeSpec(const EdgeRef& ref);
  [[nodiscard]] EdgeSpec* FindEdgeSpecByName(std::string_view name);

  [[nodiscard]] std::uint64_t GenerateNextBatchHolderId() {
    return batch_holder_id_counter_.fetch_add(1);
  }

  std::atomic<std::uint64_t> batch_holder_id_counter_{0};
  std::vector<Operator*> registered_operators_;
  std::map<std::int32_t, Operator*> operator_lookup_table_;
  std::map<std::int32_t, std::set<Edge>> outgoing_edges_;
  std::map<std::int32_t, std::set<Edge>> incoming_edges_;
  std::map<std::pair<std::int32_t, std::int32_t>, EdgeConfig> edge_configs_;
  std::vector<EdgeSpec> edge_specs_;
  std::vector<EdgeTuningPolicy> tuning_policies_;
  std::shared_ptr<memory::MemoryResourceManager> memory_resource_manager_;
};

// =============================================================================
// EdgeTuner — structural retune on Built Graph (§6.3)
// =============================================================================

/// @brief Fluent structural tuner returned by Graph::Tune().
class EdgeTuner {
 public:
  EdgeTuner(Graph* graph, Graph::EdgeSpec* spec) noexcept : graph_(graph), spec_(spec) {}

  EdgeTuner& Full() noexcept;
  EdgeTuner& Streaming() noexcept;
  EdgeTuner& Concatenating() noexcept;
  EdgeTuner& Partitioned(std::size_t partition_count) noexcept;
  EdgeTuner& PartitionedUpTo(std::size_t max_partitions) noexcept;
  EdgeTuner& FlowControl(std::size_t max_batches, std::size_t max_bytes = kUnlimited) noexcept;
  EdgeTuner& Spill(SpillPolicy policy) noexcept;
  EdgeTuner& Config(EdgeConfig config) noexcept;

 private:
  void SyncSideMap() noexcept;

  Graph* graph_;
  Graph::EdgeSpec* spec_;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_GRAPH_HPP_
