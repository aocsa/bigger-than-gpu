#ifndef QUEST_TASKFLOW_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATOR_HPP_

/// @file operator.hpp
/// @brief Base Operator class and SimpleOperator template for the execution graph.

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

/// @brief Pair type for referencing a specific named port on an operator.
using OperatorPortPair = std::pair<class Operator*, std::string>;

// =============================================================================
// Operator Base Class
// =============================================================================

/// @brief Abstract base class for all operators in the execution graph.
class Operator {
 public:
  Operator(std::uint32_t operator_id, std::string expression_string, OperatorType operator_type);

  virtual ~Operator() = default;

  Operator(const Operator&) = delete;
  Operator& operator=(const Operator&) = delete;
  Operator(Operator&&) noexcept = delete;
  Operator& operator=(Operator&&) noexcept = delete;

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
  [[nodiscard]] virtual std::vector<std::string_view> DeclaredInputPorts() const { return {}; }

  /// @brief Required EdgeConfig for the given input port (config resolution layer 4).
  ///
  /// The author's explicit Edge/Chain calls (layer 3) override this; explicit
  /// contradictions without .Config() are a Build() error.
  [[nodiscard]] virtual EdgeConfig InputEdgeContract(std::string_view /*port*/) const { return {}; }

  /// @brief Estimates output row count. Default assumes 1:1 ratio with input.
  [[nodiscard]] virtual std::pair<bool, std::uint64_t> GetEstimatedOutputNumRows() const;

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

  /// @brief The execute role: passthrough by default; correct for operators
  ///        whose work happens in their source or sink role.
  [[nodiscard]] virtual Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input);

  /// @brief Default consults OperatorType; blocking operators override once finalized.
  [[nodiscard]] virtual bool IsSource() const noexcept {
    return is_source_operator(operator_type_);
  }

  /// @brief When true, the pipeline drain loop stops after the current RunTask.
  ///
  /// LimitOperator returns true once remaining rows hit 0. Cross-barrier
  /// cancellation is out of scope here.
  [[nodiscard]] virtual bool ShouldStopSource() const noexcept { return false; }

  /// @brief The source role: end-of-stream returns engaged Result<nullopt>.
  ///
  /// Default returns an error — only sources and finalized blocking operators
  /// may be asked for batches.
  [[nodiscard]] virtual Result<std::optional<cache::DataBlockPtr>> GetNextBatch();

  /// @brief Default: blocking operators are sinks; terminal collectors should override.
  [[nodiscard]] virtual bool IsSink() const noexcept { return IsBlocking(); }

  /// @brief The sink role: blocking operators accumulate state here.
  ///
  /// Default returns an error — only sinks may be fed final batches.
  [[nodiscard]] virtual Status Sink(cache::DataBlockPtr batch);

  /// @brief Called exactly once when the producer pipeline completes.
  ///
  /// Flips a blocking operator from sink to source mode (e.g. hash table
  /// built → start emitting). Default is a no-op.
  [[nodiscard]] virtual Status FinalizeSink();

  // ===========================================================================
  // Identity and Type
  // ===========================================================================

  [[nodiscard]] std::uint32_t id() const noexcept { return operator_id_; }
  [[nodiscard]] OperatorType type() const noexcept { return operator_type_; }
  void set_type(OperatorType new_type) noexcept { operator_type_ = new_type; }

  [[nodiscard]] std::string_view expression() const noexcept { return expression_string_; }

  /// @brief Unique log/debug identifier: "{operator_type}_{operator_id}".
  [[nodiscard]] std::string MessageId() const;

  // ===========================================================================
  // Port Access
  // ===========================================================================

  [[nodiscard]] Port& input() noexcept { return input_port_; }
  [[nodiscard]] const Port& input() const noexcept { return input_port_; }
  [[nodiscard]] Port& output() noexcept { return output_port_; }
  [[nodiscard]] const Port& output() const noexcept { return output_port_; }

  [[nodiscard]] OperatorPortPair operator[](const std::string& port_name) {
    return {this, port_name};
  }

  // ===========================================================================
  // Default Cache Access (convenience methods)
  // ===========================================================================

  /// @brief Returns the primary input cache; multi-input operators use input()[port] instead.
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder> InputCache();

  [[nodiscard]] Status AddToOutput(std::shared_ptr<cache::DataBlock> data,
                                   const std::string& cache_id = "");

  // ===========================================================================
  // Input Status
  // ===========================================================================

  [[nodiscard]] bool InputAllFinished() const { return input_port_.AllFinished(); }

  [[nodiscard]] std::size_t TotalInputRowsAdded() const { return input_port_.TotalRowsAdded(); }

  [[nodiscard]] bool InputCacheFinished(const std::string& port_name) const {
    return input_port_.IsFinished(port_name);
  }

  [[nodiscard]] std::size_t InputCacheNumRowsAdded(const std::string& port_name) const {
    return input_port_.RowsAdded(port_name);
  }

  // ===========================================================================
  // Graph Relationship
  // ===========================================================================

  void set_parent(std::int32_t parent_operator_id) noexcept {
    parent_operator_id_ = parent_operator_id;
  }

  [[nodiscard]] bool has_parent() const noexcept { return parent_operator_id_ >= 0; }

  [[nodiscard]] std::int32_t parent_id() const noexcept { return parent_operator_id_; }

  /// @brief Called automatically when the operator is added to a graph.
  void set_graph(std::shared_ptr<Graph> owning_graph) { owning_graph_ = owning_graph; }

  [[nodiscard]] std::shared_ptr<Graph> graph() const { return owning_graph_.lock(); }

 protected:
  std::uint32_t operator_id_;
  std::string expression_string_;
  OperatorType operator_type_;
  Port input_port_{this};
  Port output_port_{this};
  std::int32_t parent_operator_id_{-1};
  std::weak_ptr<Graph> owning_graph_;  // weak to avoid circular ownership
};

// =============================================================================
// Simple Operator Template
// =============================================================================

/// @brief Base for stateless pipeline operators such as Filter, Project, and Cast.
class SimpleOperator : public Operator {
 public:
  using Operator::Operator;

 protected:
  // Redeclared pure so subclasses cannot accidentally inherit the passthrough default.
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override = 0;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATOR_HPP_
