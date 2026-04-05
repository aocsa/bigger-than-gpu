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
/// - Building the graph topology using a fluent DSL (graph += scan >> filter)
/// - Managing operator lifecycle and connections
/// - Scheduling parallel execution while respecting data dependencies
/// - Tracking execution state and coordinating thread completion
///
/// Example usage:
/// @code
///   auto graph = std::make_shared<Graph>();
///   *graph += table_scan >> filter >> project >> output;
///   graph->execute();
/// @endcode

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <thread>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/taskflow/kpair.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/result.hpp"

namespace quest::taskflow {

// =============================================================================
// Graph Edge Representation
// =============================================================================

/// @brief Represents a directed edge in the execution graph.
///
/// Each edge connects a source operator's output port to a target operator's
/// input port. Edges are stored in adjacency lists for efficient graph
/// traversal during execution scheduling.
struct Edge {
  // The unique identifier of the source operator producing data.
  std::int32_t source_operator_id;

  // The unique identifier of the target operator consuming data.
  std::int32_t target_operator_id;

  // Name of the output port on the source operator.
  // Empty string indicates the default port.
  std::string source_port_name;

  // Name of the input port on the target operator.
  // Empty string indicates the default port.
  std::string target_port_name;

  /// @brief Compare edges for ordering in std::set containers.
  ///
  /// Edges are ordered first by target, then by source. This ordering
  /// optimizes lookups when finding all incoming edges to a target.
  [[nodiscard]] auto operator<=>(const Edge& other) const noexcept {
    if (target_operator_id != other.target_operator_id) {
      return target_operator_id <=> other.target_operator_id;
    }
    return source_operator_id <=> other.source_operator_id;
  }

  /// @brief Check equality based on source and target identifiers.
  [[nodiscard]] bool operator==(const Edge& other) const noexcept {
    return source_operator_id == other.source_operator_id &&
           target_operator_id == other.target_operator_id;
  }
};

// =============================================================================
// Graph Class
// =============================================================================

/// @brief Execution graph managing operators and their data flow connections.
///
/// The Graph class implements a directed acyclic graph (DAG) where:
/// - Nodes are Operator instances representing computation units
/// - Edges are BatchHolder connections carrying data between operators
///
/// The graph supports a fluent domain-specific language (DSL) for construction:
/// @code
///   *graph += scan >> filter;           // Default ports
///   *graph += scan_a >> join["left"];   // Named input port
///   *graph += link(a, b, config);       // With edge configuration
/// @endcode
///
/// During execution, the graph performs topological scheduling to ensure
/// operators run only after their dependencies are satisfied, while maximizing
/// parallelism for independent operators.
class Graph : public std::enable_shared_from_this<Graph> {
 public:
  /// @brief Constructs an empty execution graph.
  ///
  /// @param memory_manager Optional memory manager for controlling memory
  ///        allocation and disk spilling. If null, a default manager is used.
  explicit Graph(
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  ~Graph() = default;

  // Non-copyable and non-movable due to internal state (atomic counters,
  // operator pointers, enable_shared_from_this).
  Graph(const Graph&) = delete;
  Graph& operator=(const Graph&) = delete;
  Graph(Graph&&) = delete;
  Graph& operator=(Graph&&) = delete;

  // ===========================================================================
  // Graph Construction API
  // ===========================================================================

  /// @brief Adds an edge to the graph using the fluent DSL.
  ///
  /// This operator enables the intuitive syntax for connecting operators:
  /// @code
  ///   *graph += scan >> filter >> project;
  /// @endcode
  ///
  /// @param edge_pair The KPair describing source and target operators.
  /// @return Reference to the stored edge for potential chaining.
  KPair& operator+=(KPair edge_pair);

  /// @brief Registers an operator as a node in the graph.
  ///
  /// Operators are automatically registered when edges are added, but this
  /// method allows explicit registration for operators without connections.
  ///
  /// @param node_operator Pointer to the operator to add (must outlive the graph).
  /// @return The operator's unique identifier.
  std::size_t AddNode(Operator* node_operator);

  /// @brief Creates a directed edge between two operators.
  ///
  /// This method:
  /// - Registers both operators if not already present
  /// - Creates batch holders for data transfer based on the configuration
  /// - Establishes parent-child relationships for dependency tracking
  /// - Connects source output ports to target input ports
  ///
  /// @param source_operator The operator producing data.
  /// @param target_operator The operator consuming data.
  /// @param source_port_name Name of the output port (or empty for default).
  /// @param target_port_name Name of the input port (or empty for default).
  /// @param edge_config Configuration for the batch holder connection.
  void AddEdge(Operator* source_operator,
               Operator* target_operator,
               const std::string& source_port_name,
               const std::string& target_port_name,
               const EdgeConfig& edge_config);

  // ===========================================================================
  // Graph Execution API
  // ===========================================================================

  /// @brief Executes all operators in the graph respecting dependencies.
  ///
  /// The execution algorithm:
  /// - Identifies root operators (those with no incoming edges)
  /// - Uses a work queue to schedule operators when dependencies are met
  /// - Launches each operator in a separate thread for parallelism
  /// - Waits for all threads to complete before returning
  ///
  /// @return Status::OK() on successful completion, error status otherwise.
  [[nodiscard]] Status Execute();

  /// @brief Finalizes the graph structure before execution.
  ///
  /// This method identifies terminal operators (those with no outgoing edges)
  /// and marks them appropriately for proper execution tracking.
  void FinalizeWorkflow();

  /// @brief Checks for and optimizes simple scan-limit query patterns.
  ///
  /// When a query consists only of TableScan -> Limit -> Output, this
  /// optimization pushes the limit down to the scan operator to avoid
  /// reading unnecessary data.
  void OptimizeScanWithLimit();

  // ===========================================================================
  // Graph Query API
  // ===========================================================================

  /// @brief Retrieves an operator by its unique identifier.
  ///
  /// @param operator_id The ID of the operator to find.
  /// @return Pointer to the operator, or nullptr if not found.
  [[nodiscard]] Operator* GetNode(std::size_t operator_id) const;

  /// @brief Gets the most recently added operator.
  ///
  /// Useful for building chains where you need a reference to the last
  /// operator added without storing it separately.
  ///
  /// @return Reference to the last operator.
  /// @throws std::runtime_error if the graph is empty.
  [[nodiscard]] Operator& GetLastOperator();
  [[nodiscard]] const Operator& GetLastOperator() const;

  /// @brief Returns the total number of operators in the graph.
  [[nodiscard]] std::size_t GetNodeCount() const noexcept {
    return registered_operators_.size();
  }

  /// @brief Finds all outgoing edges from an operator.
  ///
  /// @param from_operator Pointer to the source operator.
  /// @return Set of edges originating from this operator.
  [[nodiscard]] std::set<Edge> GetOutgoingEdges(Operator* from_operator) const;
  [[nodiscard]] std::set<Edge> GetOutgoingEdges(std::int32_t operator_id) const;

  /// @brief Finds all incoming edges to an operator.
  ///
  /// @param to_operator Pointer to the target operator.
  /// @return Set of edges pointing to this operator.
  [[nodiscard]] std::set<Edge> GetIncomingEdges(Operator* to_operator) const;
  [[nodiscard]] std::set<Edge> GetIncomingEdges(std::int32_t operator_id) const;

  /// @brief Estimates the number of input rows for an operator.
  ///
  /// Used for query optimization and progress tracking. Returns an estimate
  /// based on upstream operator statistics when available.
  ///
  /// @param operator_id The target operator's ID.
  /// @return Pair of (is_estimate_available, estimated_row_count).
  [[nodiscard]] std::pair<bool, std::uint64_t> EstimateInputRowCount(
      std::int32_t operator_id) const;

  /// @brief Estimates input rows for a specific port of an operator.
  ///
  /// Useful for multi-input operators like joins where each input port
  /// may have different cardinality.
  ///
  /// @param operator_id The target operator's ID.
  /// @param port_name The specific input port name.
  /// @return Pair of (is_estimate_available, estimated_row_count).
  [[nodiscard]] std::pair<bool, std::uint64_t> EstimateInputRowCountForPort(
      std::int32_t operator_id,
      const std::string& port_name) const;

  // ===========================================================================
  // Debug and Visualization API
  // ===========================================================================

  /// @brief Prints the graph structure to standard output.
  ///
  /// Displays all operators with their types and IDs, followed by
  /// the edge connections in a human-readable format.
  void PrintStructure() const;

  /// @brief Prints the graph backwards from a specific operator.
  ///
  /// Useful for debugging to see the dependency chain leading to
  /// a particular operator.
  ///
  /// @param operator_id Starting point for backward traversal.
  void PrintBackwardsFrom(std::int32_t operator_id) const;

  // ===========================================================================
  // Memory Management API
  // ===========================================================================

  /// @brief Gets the memory resource manager for this graph.
  [[nodiscard]] std::shared_ptr<memory::MemoryResourceManager>
  GetMemoryManager() const {
    return memory_resource_manager_;
  }

  /// @brief Sets the memory resource manager.
  ///
  /// @param manager The new memory manager to use.
  void SetMemoryManager(
      std::shared_ptr<memory::MemoryResourceManager> manager) {
    memory_resource_manager_ = std::move(manager);
  }

 private:
  // Sentinel ID representing the virtual entry point of the graph.
  // Root operators (sources) are connected from this sentinel node.
  static constexpr std::int32_t kGraphEntryPointId = -1;

  /// @brief Creates batch holders for an edge based on configuration.
  ///
  /// For partitioned edges, creates multiple batch holders (one per partition).
  /// For simple edges, creates a single batch holder.
  ///
  /// @param edge_config The configuration specifying holder type and count.
  /// @return Vector of created batch holders.
  [[nodiscard]] std::vector<std::shared_ptr<cache::IBatchHolder>>
  CreateBatchHolders(const EdgeConfig& edge_config);

  /// @brief Generates a unique ID for a new batch holder.
  [[nodiscard]] std::uint64_t GenerateNextBatchHolderId() {
    return batch_holder_id_counter_.fetch_add(1);
  }

  // Counter for generating unique batch holder identifiers.
  std::atomic<std::uint64_t> batch_holder_id_counter_{0};

  // Ordered list of operators for iteration (maintains insertion order).
  std::vector<Operator*> registered_operators_;

  // Map from operator ID to operator pointer for O(1) lookup.
  std::map<std::int32_t, Operator*> operator_lookup_table_;

  // Adjacency list: operator_id -> set of outgoing edges.
  std::map<std::int32_t, std::set<Edge>> outgoing_edges_;

  // Reverse adjacency list: operator_id -> set of incoming edges.
  std::map<std::int32_t, std::set<Edge>> incoming_edges_;

  // Stores the most recently added edge for fluent DSL chaining.
  std::unique_ptr<KPair> most_recent_edge_;

  // Memory manager controlling allocation and disk spilling.
  std::shared_ptr<memory::MemoryResourceManager> memory_resource_manager_;
};

// =============================================================================
// Type Aliases
// =============================================================================

/// @brief Thread type used for parallel operator execution.
///
/// Uses std::jthread for automatic joining on destruction, preventing
/// resource leaks if exceptions occur during execution.
using ExecutionThread = std::jthread;

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_GRAPH_HPP_
