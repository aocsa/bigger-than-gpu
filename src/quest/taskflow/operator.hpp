#ifndef QUEST_TASKFLOW_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATOR_HPP_

/// @file operator.hpp
/// @brief Base Operator class representing a computation unit in the execution graph.
///
/// An Operator is the fundamental building block of query execution. Each operator
/// represents a specific computation (scan, filter, project, join, etc.) and is
/// connected to other operators via input and output ports.
///
/// Key concepts:
/// - Input Port: Receives data batches from upstream operators via BatchHolders
/// - Output Port: Sends processed data batches to downstream operators
/// - Expression: A string describing the logical operation (for debugging/logging)
/// - Run method: The core computation logic executed during graph execution
///
/// Operators form a directed acyclic graph (DAG) where data flows from source
/// operators (like TableScan) through transformation operators (like Filter,
/// Project) to sink operators (like Output).
///
/// Example derived operator:
/// @code
///   class FilterOperator : public SimpleOperator {
///   public:
///     FilterOperator(std::uint32_t id, std::string predicate)
///         : SimpleOperator(id, predicate, OperatorType::Filter) {}
///
///   protected:
///     Result<std::shared_ptr<DataBlock>> ProcessBatch(
///         std::shared_ptr<DataBlock> input) override {
///       // Apply filter predicate to input batch
///       return filtered_batch;
///     }
///   };
/// @endcode

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/operator_type.hpp"
#include "quest/taskflow/port.hpp"

namespace quest::taskflow {

// Forward declarations
class Graph;

/// @brief Execution status returned by an operator's run() method.
///
/// Indicates whether the operator completed successfully or encountered
/// an error that requires stopping execution.
enum class ExecutionStatus : std::uint8_t {
  // Operator encountered an error and must stop execution.
  // The graph scheduler will not proceed with downstream operators.
  Stop,

  // Operator completed successfully.
  // The graph scheduler will continue with downstream operators.
  Proceed
};

/// @brief Pair type for referencing a specific port on an operator.
///
/// Used in the graph DSL to specify named ports for multi-input operators:
/// @code
///   graph += scan_a >> join["left_input"];
///   graph += scan_b >> join["right_input"];
/// @endcode
using OperatorPortPair = std::pair<class Operator*, std::string>;

// =============================================================================
// Operator Base Class
// =============================================================================

/// @brief Abstract base class for all operators in the execution graph.
///
/// Derived classes must implement:
/// - run(): Execute the operator's computation
/// - can_you_throttle_my_input(): Indicate if backpressure is supported
///
/// The Operator class provides:
/// - Input and output port management
/// - Graph relationship tracking (parent operators)
/// - Execution state management (done flag)
/// - Convenience methods for data I/O
class Operator {
 public:
  /// @brief Constructs an operator with the given identity and expression.
  ///
  /// @param operator_id Unique identifier for this operator within the graph.
  /// @param expression_string The logical expression this operator implements
  ///        (e.g., "column_a > 10" for a filter operator).
  /// @param operator_type The type of operator (determines scheduling behavior).
  Operator(std::uint32_t operator_id,
           std::string expression_string,
           OperatorType operator_type);

  virtual ~Operator() = default;

  // Non-copyable due to unique identity and port ownership.
  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;

  // Movable for container storage.
  Operator(Operator&&) noexcept = delete;
  Operator& operator=(Operator&&) noexcept = delete;

  // ===========================================================================
  // Core Interface (must be implemented by derived classes)
  // ===========================================================================

  /// @brief Executes the operator's computation.
  ///
  /// This method is called by the graph scheduler when all dependencies are
  /// satisfied. Implementations should:
  /// - Pull data from input ports
  /// - Process the data according to the operator's logic
  /// - Push results to output ports
  ///
  /// @return ExecutionStatus::Proceed on success, ExecutionStatus::Stop on failure.
  [[nodiscard]] virtual ExecutionStatus run() = 0;

  /// @brief Indicates whether this operator supports input throttling.
  ///
  /// When true, upstream operators can apply backpressure by waiting when
  /// this operator's input buffers are full. This enables flow control to
  /// prevent memory exhaustion in long pipelines.
  ///
  /// @return true if backpressure is supported, false otherwise.
  [[nodiscard]] virtual bool can_you_throttle_my_input() const = 0;

  // ===========================================================================
  // Port Contracts (§4.1 of graph-builder-dsl.md)
  //
  // Operators may declare which named input ports they accept and what
  // EdgeConfig each port requires. GraphBuilder::Build() validates edges
  // against these declarations, so a typo in a port name becomes a compile
  // error rather than a silent new port.
  //
  // Operators that leave these at their defaults are treated as "undeclared" —
  // the legacy behavior: any port name is allowed and no contract is checked.
  // ===========================================================================

  /// @brief Input ports this operator accepts.
  ///
  /// Empty means undeclared (any name allowed — legacy behavior).
  /// GraphBuilder::Build() validates edge target port names against this list.
  [[nodiscard]] virtual std::vector<std::string_view> DeclaredInputPorts() const {
    return {};
  }

  /// @brief Required EdgeConfig for the given input port.
  ///
  /// Called by GraphBuilder::Build() (config resolution layer 4) to apply
  /// per-port constraints. The author's explicit Edge/Chain calls (layer 3)
  /// override this; explicit contradictions without .Config() are a Build() error.
  /// Default: returns EdgeConfig{} (no constraint — streaming, simple holder).
  [[nodiscard]] virtual EdgeConfig InputEdgeContract(std::string_view /*port*/) const {
    return {};
  }

  /// @brief Estimates the number of output rows this operator will produce.
  ///
  /// Used by the query optimizer for cardinality estimation and by the
  /// progress tracker for user feedback. The default implementation
  /// assumes 1:1 row ratio (output rows = input rows).
  ///
  /// @return Pair of (is_estimate_available, estimated_row_count).
  [[nodiscard]] virtual std::pair<bool, std::uint64_t>
  GetEstimatedOutputNumRows() const;

  /// @brief Indicates whether this operator is a pipeline breaker.
  ///
  /// A blocking operator must consume (part of) its input before producing
  /// output (aggregate, sort, join, partition). During pipeline construction
  /// it becomes the sink of the producer pipeline and the source of the
  /// consumer pipeline. The default consults the OperatorType classification;
  /// override for operators whose behavior differs from their type's default
  /// (mirrors Sirius's overridable build_pipelines()).
  [[nodiscard]] virtual bool IsBlocking() const noexcept {
    return is_blocking_operator(operator_type_);
  }

  // ===========================================================================
  // Pipeline Roles (mirrors sirius_physical_operator's execute/source/sink)
  //
  // In pipeline execution one operator plays up to three roles:
  // - execute: pure batch-in -> batch-out, called by the pipeline driver on
  //   every streaming operator in plan order (push loop).
  // - source:  decides whether a new task can exist and supplies its input
  //   batch. Sources are scans and blocking operators that have finished
  //   accumulating (post-FinalizeSink).
  // - sink:    receives the pipeline's final batch. Blocking operators
  //   accumulate state here; FinalizeSink() flips them into source mode.
  // ===========================================================================

  /// @brief Processes a single batch (the execute role).
  ///
  /// Called by the pipeline driver on each operator in plan order. The
  /// default implementation is a passthrough (returns the input unchanged),
  /// which is correct for operators whose work happens in their source or
  /// sink role.
  ///
  /// @param input The input data block.
  /// @return The transformed data block, or an error status.
  [[nodiscard]] virtual Result<cache::DataBlockPtr> ProcessBatch(
      cache::DataBlockPtr input);

  /// @brief Indicates whether this operator can act as a pipeline source.
  ///
  /// Default consults the OperatorType classification (TableScan,
  /// BindableTableScan, Generate). Blocking operators should report true
  /// once finalized if they re-emit accumulated state.
  [[nodiscard]] virtual bool IsSource() const noexcept {
    return is_source_operator(operator_type_);
  }

  /// @brief When true, the pipeline drain loop stops claiming further source
  ///        batches after the current RunTask (single-pipeline early stop).
  ///
  /// Default false. LimitOperator returns true once remaining rows hit 0.
  /// Multi-pipeline / cross-barrier cancellation is out of scope here.
  [[nodiscard]] virtual bool ShouldStopSource() const noexcept { return false; }

  /// @brief Produces the next batch from this source (the source role).
  ///
  /// End-of-stream is a value, not an error: an exhausted source returns
  /// an engaged Result holding std::nullopt. The default implementation
  /// returns an error — only sources (and finalized blocking operators)
  /// may be asked for batches.
  ///
  /// @return next batch | std::nullopt on end-of-stream | error status.
  [[nodiscard]] virtual Result<std::optional<cache::DataBlockPtr>>
  GetNextBatch();

  /// @brief Indicates whether this operator is a pipeline sink.
  ///
  /// Default: blocking operators are sinks (they accumulate their input
  /// before producing output). Terminal collectors should override to true.
  [[nodiscard]] virtual bool IsSink() const noexcept { return IsBlocking(); }

  /// @brief Receives one final batch of the pipeline (the sink role).
  ///
  /// Blocking operators accumulate state here (hash table, sort buffer).
  /// The default implementation returns an error — only sinks may be fed.
  ///
  /// @param batch The batch arriving at the end of the pipeline.
  [[nodiscard]] virtual Status Sink(cache::DataBlockPtr batch);

  /// @brief Called exactly once when the producer pipeline completes.
  ///
  /// Flips a blocking operator from sink mode to source mode (e.g. hash
  /// table built -> start emitting). Default is a no-op returning OK so
  /// non-sink operators are unaffected.
  [[nodiscard]] virtual Status FinalizeSink();

  // ===========================================================================
  // Identity and Type
  // ===========================================================================

  /// @brief Gets the unique operator identifier.
  [[nodiscard]] std::uint32_t id() const noexcept { return operator_id_; }

  /// @brief Gets the operator type (determines scheduling behavior).
  [[nodiscard]] OperatorType type() const noexcept { return operator_type_; }

  /// @brief Sets the operator type (for dynamic reconfiguration).
  void set_type(OperatorType new_type) noexcept { operator_type_ = new_type; }

  /// @brief Gets the expression string describing this operator's logic.
  [[nodiscard]] std::string_view expression() const noexcept {
    return expression_string_;
  }

  /// @brief Generates a unique message identifier for logging and debugging.
  ///
  /// Format: "{operator_type}_{operator_id}"
  [[nodiscard]] std::string MessageId() const;

  // ===========================================================================
  // Port Access
  // ===========================================================================

  /// @brief Gets the input port for receiving data from upstream operators.
  [[nodiscard]] Port& input() noexcept { return input_port_; }
  [[nodiscard]] const Port& input() const noexcept { return input_port_; }

  /// @brief Gets the output port for sending data to downstream operators.
  [[nodiscard]] Port& output() noexcept { return output_port_; }
  [[nodiscard]] const Port& output() const noexcept { return output_port_; }

  /// @brief Creates a named port reference for graph DSL connections.
  ///
  /// @param port_name The name of the port to reference.
  /// @return Pair of (this operator, port_name) for use in graph += syntax.
  ///
  /// @code
  ///   graph += source >> join["left_input"];
  /// @endcode
  [[nodiscard]] OperatorPortPair operator[](const std::string& port_name) {
    return {this, port_name};
  }

  // ===========================================================================
  // Default Cache Access (convenience methods)
  // ===========================================================================

  /// @brief Gets the default input batch holder.
  ///
  /// For single-input operators, this returns the primary input cache.
  /// Multi-input operators should use input()["port_name"] instead.
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder> InputCache();

  /// @brief Gets an output batch holder by cache identifier.
  ///
  /// @param cache_id The cache identifier (empty for default output).
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder> OutputCache(
      const std::string& cache_id = "");

  /// @brief Adds processed data to the output cache.
  ///
  /// @param data The cache data to send downstream.
  /// @param cache_id The cache identifier (empty for default output).
  /// @return Status::OK() on success, error status on failure.
  [[nodiscard]] Status AddToOutput(std::shared_ptr<cache::DataBlock> data,
                                    const std::string& cache_id = "");

  /// @brief Blocks if the output cache is saturated (flow control).
  ///
  /// This method implements backpressure by waiting until downstream
  /// operators have consumed enough data to make room for more output.
  ///
  /// @param cache_id The cache identifier (empty for default output).
  void WaitIfOutputSaturated(const std::string& cache_id = "");

  // ===========================================================================
  // Input Status
  // ===========================================================================

  /// @brief Checks if all input caches have been marked as finished.
  ///
  /// An input is finished when the upstream operator has completed and
  /// called finish() on its output port.
  [[nodiscard]] bool InputAllFinished() const {
    return input_port_.AllFinished();
  }

  /// @brief Gets the total number of rows received across all input ports.
  [[nodiscard]] std::size_t TotalInputRowsAdded() const {
    return input_port_.TotalRowsAdded();
  }

  /// @brief Checks if a specific input port's cache is finished.
  ///
  /// @param port_name The name of the input port to check.
  [[nodiscard]] bool InputCacheFinished(const std::string& port_name) const {
    return input_port_.IsFinished(port_name);
  }

  /// @brief Gets the number of rows added to a specific input port.
  ///
  /// @param port_name The name of the input port.
  [[nodiscard]] std::size_t InputCacheNumRowsAdded(
      const std::string& port_name) const {
    return input_port_.RowsAdded(port_name);
  }

  // ===========================================================================
  // Graph Relationship
  // ===========================================================================

  /// @brief Sets the parent operator identifier for dependency tracking.
  ///
  /// @param parent_operator_id The ID of the upstream operator (-1 for root).
  void set_parent(std::int32_t parent_operator_id) noexcept {
    parent_operator_id_ = parent_operator_id;
  }

  /// @brief Checks if this operator has a parent (is not a root/source).
  [[nodiscard]] bool has_parent() const noexcept {
    return parent_operator_id_ >= 0;
  }

  /// @brief Gets the parent operator identifier (-1 if this is a root).
  [[nodiscard]] std::int32_t parent_id() const noexcept {
    return parent_operator_id_;
  }

  /// @brief Associates this operator with its owning graph.
  ///
  /// Called automatically when the operator is added to a graph.
  void set_graph(std::shared_ptr<Graph> owning_graph) {
    owning_graph_ = owning_graph;
  }

  /// @brief Gets the graph that owns this operator.
  [[nodiscard]] std::shared_ptr<Graph> graph() const {
    return owning_graph_.lock();
  }

  // ===========================================================================
  // Execution State
  // ===========================================================================

  /// @brief Checks if this operator has completed execution.
  [[nodiscard]] bool is_done() const noexcept {
    return execution_completed_.load();
  }

  /// @brief Marks this operator as having completed execution.
  ///
  /// Called by the graph scheduler after run() returns Proceed.
  void mark_done() noexcept { execution_completed_.store(true); }

 protected:
  // Unique identifier for this operator within the graph.
  std::uint32_t operator_id_;

  // String representation of the operator's logical expression.
  std::string expression_string_;

  // Type classification for scheduling and optimization.
  OperatorType operator_type_;

  // Port for receiving data from upstream operators.
  Port input_port_{this};

  // Port for sending data to downstream operators.
  Port output_port_{this};

  // ID of the parent operator (-1 for root/source operators).
  std::int32_t parent_operator_id_{-1};

  // Weak reference to the owning graph (avoids circular dependency).
  std::weak_ptr<Graph> owning_graph_;

  // Atomic flag indicating execution completion (thread-safe).
  std::atomic<bool> execution_completed_{false};
};

// =============================================================================
// Simple Operator Template
// =============================================================================

/// @brief Base class for simple operators that transform input to output.
///
/// SimpleOperator provides a standard run() implementation that:
/// - Pulls batches from the input port
/// - Calls ProcessBatch() for each batch
/// - Pushes results to the output port
/// - Handles flow control automatically
///
/// Use this base class for stateless transformation operators like:
/// - Filter: Removes rows not matching a predicate
/// - Project: Selects or transforms columns
/// - Cast: Converts data types
///
/// Example:
/// @code
///   class FilterOperator : public SimpleOperator {
///   public:
///     FilterOperator(std::uint32_t id, std::string predicate)
///         : SimpleOperator(id, predicate, OperatorType::Filter) {}
///
///   protected:
///     Result<std::shared_ptr<DataBlock>> ProcessBatch(
///         std::shared_ptr<DataBlock> input) override {
///       // Filter implementation
///     }
///   };
/// @endcode
class SimpleOperator : public Operator {
 public:
  using Operator::Operator;

  /// @brief Simple operators always support input throttling.
  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  /// @brief Standard batch processing loop.
  ///
  /// Pulls batches from input, processes each via ProcessBatch(),
  /// and pushes results to output with flow control.
  [[nodiscard]] ExecutionStatus run() override;

 protected:
  /// @brief Processes a single input batch.
  ///
  /// Redeclared pure: SimpleOperator subclasses MUST implement their
  /// transformation logic (the Operator base default is a passthrough,
  /// which would silently do nothing useful here).
  ///
  /// @param input The input cache data batch.
  /// @return The transformed cache data, or an error status.
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(
      cache::DataBlockPtr input) override = 0;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATOR_HPP_
