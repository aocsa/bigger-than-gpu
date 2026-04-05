#ifndef QUEST_TASKFLOW_KPAIR_HPP_
#define QUEST_TASKFLOW_KPAIR_HPP_

/// @file kpair.hpp
/// @brief KPair (Kernel Pair) for linking operators in the execution graph.
///
/// This file provides the fluent DSL (Domain-Specific Language) for connecting
/// operators in an execution graph. The DSL allows intuitive graph construction:
///
/// @code
///   // Simple linear pipeline
///   graph += table_scan >> filter >> project >> output;
///
///   // Multi-input operator (join)
///   graph += table_scan_a >> join["left_input"];
///   graph += table_scan_b >> join["right_input"];
///   graph += join >> output;
///
///   // With edge configuration
///   graph += link(source, sink, EdgeConfig::Concatenating()
///       .with_flow_control(10, 1024 * 1024));
/// @endcode
///
/// The >> operator creates KPair objects that capture the source and destination
/// operators along with any port names and configuration. The Graph::operator+=
/// then processes these pairs to create actual edges with batch holders.

#include <limits>
#include <string>
#include <utility>

#include "quest/cache/batch_holder.hpp"

namespace quest::taskflow {

// Forward declaration
class Operator;

/// @brief Pair type for operator with optional port name.
///
/// Used to specify named ports when connecting to multi-input operators.
using OperatorPortPair = std::pair<Operator*, std::string>;

// =============================================================================
// Edge Configuration
// =============================================================================

/// @brief Configuration for the batch holder connection between two operators.
///
/// EdgeConfig controls how data flows between operators:
/// - Type: Simple (individual batches) or Concatenating (merged batches)
/// - Partitioning: Number of parallel data streams
/// - Flow control: Backpressure thresholds to prevent memory exhaustion
struct EdgeConfig {
  // Type of batch holder (affects how data is delivered to consumers).
  cache::BatchHolderType type{cache::BatchHolderType::kSimple};

  // Number of partitions for parallel processing patterns.
  // Values > 1 create multiple batch holders for the FOR_EACH pattern.
  std::size_t num_partitions{1};

  // Maximum number of batches before applying backpressure.
  // Default is unlimited (max size_t value).
  std::size_t flow_control_batches_threshold{
      std::numeric_limits<std::size_t>::max()};

  // Maximum bytes before applying backpressure.
  // Default is unlimited (max size_t value).
  std::size_t flow_control_bytes_threshold{
      std::numeric_limits<std::size_t>::max()};

  /// @brief Creates a configuration for simple (per-batch) delivery.
  ///
  /// Each batch is delivered individually to the consumer.
  [[nodiscard]] static EdgeConfig Simple() {
    return EdgeConfig{.type = cache::BatchHolderType::kSimple};
  }

  /// @brief Creates a configuration for concatenating delivery.
  ///
  /// All batches are merged into a single result before delivery.
  /// Useful for operators that need all data at once (like sorts).
  [[nodiscard]] static EdgeConfig Concatenating() {
    return EdgeConfig{.type = cache::BatchHolderType::kConcatenating};
  }

  /// @brief Creates a configuration for partitioned parallel outputs.
  ///
  /// Creates multiple batch holders, one per partition. Used for
  /// parallel processing patterns where work is distributed.
  ///
  /// @param partition_count Number of partitions to create.
  [[nodiscard]] static EdgeConfig Partitioned(std::size_t partition_count) {
    return EdgeConfig{.type = cache::BatchHolderType::kSimple,
                      .num_partitions = partition_count};
  }

  /// @brief Adds flow control thresholds to the configuration.
  ///
  /// Flow control prevents memory exhaustion by having producers wait
  /// when consumers are falling behind.
  ///
  /// @param max_batches Maximum batches before blocking.
  /// @param max_bytes Maximum bytes before blocking (optional).
  /// @return Reference to this config for method chaining.
  EdgeConfig& with_flow_control(
      std::size_t max_batches,
      std::size_t max_bytes = std::numeric_limits<std::size_t>::max()) {
    flow_control_batches_threshold = max_batches;
    flow_control_bytes_threshold = max_bytes;
    return *this;
  }
};

// =============================================================================
// KPair - Kernel/Operator Pair for Graph Edges
// =============================================================================

/// @brief Represents a directed edge between two operators in the execution graph.
///
/// KPair captures all information needed to create an edge:
/// - Source operator and optional output port name
/// - Destination operator and optional input port name
/// - Edge configuration (batch holder type, partitioning, flow control)
///
/// KPairs are created by the >> operator or link() function and consumed
/// by Graph::operator+= to create actual edges with batch holders.
class KPair {
 public:
  /// @brief Constructs a KPair connecting two operators with default ports.
  ///
  /// @param source_operator The operator producing data.
  /// @param destination_operator The operator consuming data.
  /// @param edge_config Configuration for the connection.
  KPair(Operator& source_operator,
        Operator& destination_operator,
        EdgeConfig edge_config = {})
      : source_operator_(&source_operator),
        destination_operator_(&destination_operator),
        edge_config_(std::move(edge_config)) {}

  /// @brief Constructs a KPair from an operator to a named port.
  ///
  /// @param source_operator The operator producing data.
  /// @param destination_port The destination operator and port name.
  /// @param edge_config Configuration for the connection.
  KPair(Operator& source_operator,
        OperatorPortPair destination_port,
        EdgeConfig edge_config = {})
      : source_operator_(&source_operator),
        destination_operator_(destination_port.first),
        destination_port_name_(std::move(destination_port.second)),
        edge_config_(std::move(edge_config)) {}

  /// @brief Constructs a KPair from a named port to an operator.
  ///
  /// @param source_port The source operator and port name.
  /// @param destination_operator The operator consuming data.
  /// @param edge_config Configuration for the connection.
  KPair(OperatorPortPair source_port,
        Operator& destination_operator,
        EdgeConfig edge_config = {})
      : source_operator_(source_port.first),
        source_port_name_(std::move(source_port.second)),
        destination_operator_(&destination_operator),
        edge_config_(std::move(edge_config)) {}

  /// @brief Constructs a KPair between two named ports.
  ///
  /// @param source_port The source operator and port name.
  /// @param destination_port The destination operator and port name.
  /// @param edge_config Configuration for the connection.
  KPair(OperatorPortPair source_port,
        OperatorPortPair destination_port,
        EdgeConfig edge_config = {})
      : source_operator_(source_port.first),
        source_port_name_(std::move(source_port.second)),
        destination_operator_(destination_port.first),
        destination_port_name_(std::move(destination_port.second)),
        edge_config_(std::move(edge_config)) {}

  // Copyable and movable for storage in containers.
  KPair(const KPair&) = default;
  KPair& operator=(const KPair&) = default;
  KPair(KPair&&) noexcept = default;
  KPair& operator=(KPair&&) noexcept = default;

  // ===========================================================================
  // Accessors
  // ===========================================================================

  /// @brief Gets the source operator.
  [[nodiscard]] Operator* source() const noexcept { return source_operator_; }

  /// @brief Gets the destination operator.
  [[nodiscard]] Operator* destination() const noexcept {
    return destination_operator_;
  }

  /// @brief Gets the source port name (empty for default port).
  [[nodiscard]] const std::string& source_port() const noexcept {
    return source_port_name_;
  }

  /// @brief Gets the destination port name (empty for default port).
  [[nodiscard]] const std::string& destination_port() const noexcept {
    return destination_port_name_;
  }

  /// @brief Checks if a custom source port was specified.
  [[nodiscard]] bool has_custom_source() const noexcept {
    return !source_port_name_.empty();
  }

  /// @brief Checks if a custom destination port was specified.
  [[nodiscard]] bool has_custom_destination() const noexcept {
    return !destination_port_name_.empty();
  }

  /// @brief Gets the edge configuration.
  [[nodiscard]] const EdgeConfig& config() const noexcept {
    return edge_config_;
  }

 private:
  // Source operator producing data.
  Operator* source_operator_{nullptr};

  // Optional source port name (empty = default).
  std::string source_port_name_;

  // Destination operator consuming data.
  Operator* destination_operator_{nullptr};

  // Optional destination port name (empty = default).
  std::string destination_port_name_;

  // Configuration for the batch holder connection.
  EdgeConfig edge_config_;
};

// =============================================================================
// Operator Overloads for Fluent DSL
// =============================================================================

/// @brief Connects two operators with default ports.
///
/// @code
///   graph += scan >> filter;
/// @endcode
[[nodiscard]] inline KPair operator>>(Operator& source_operator,
                                      Operator& destination_operator) {
  return KPair(source_operator, destination_operator);
}

/// @brief Connects an operator to a named destination port.
///
/// @code
///   graph += scan_a >> join["left_input"];
/// @endcode
[[nodiscard]] inline KPair operator>>(Operator& source_operator,
                                      OperatorPortPair destination_port) {
  return KPair(source_operator, std::move(destination_port));
}

/// @brief Connects a named source port to an operator.
///
/// @code
///   graph += sort["sorted_output"] >> merge;
/// @endcode
[[nodiscard]] inline KPair operator>>(OperatorPortPair source_port,
                                      Operator& destination_operator) {
  return KPair(std::move(source_port), destination_operator);
}

/// @brief Connects a named source port to a named destination port.
///
/// @code
///   graph += partition["output_0"] >> worker["input_0"];
/// @endcode
[[nodiscard]] inline KPair operator>>(OperatorPortPair source_port,
                                      OperatorPortPair destination_port) {
  return KPair(std::move(source_port), std::move(destination_port));
}

// =============================================================================
// Link Functions (alternative to >> operator, allows configuration)
// =============================================================================

/// @brief Creates a link between two operators with configuration.
///
/// Use link() instead of >> when you need to specify edge configuration.
///
/// @code
///   graph += link(source, sink, EdgeConfig::Concatenating());
/// @endcode
[[nodiscard]] inline KPair link(Operator& source_operator,
                                Operator& destination_operator,
                                EdgeConfig edge_config = {}) {
  return KPair(source_operator, destination_operator, std::move(edge_config));
}

/// @brief Creates a link from an operator to a named port with configuration.
[[nodiscard]] inline KPair link(Operator& source_operator,
                                OperatorPortPair destination_port,
                                EdgeConfig edge_config = {}) {
  return KPair(
      source_operator, std::move(destination_port), std::move(edge_config));
}

/// @brief Creates a link from a named port to an operator with configuration.
[[nodiscard]] inline KPair link(OperatorPortPair source_port,
                                Operator& destination_operator,
                                EdgeConfig edge_config = {}) {
  return KPair(
      std::move(source_port), destination_operator, std::move(edge_config));
}

/// @brief Creates a link between two named ports with configuration.
[[nodiscard]] inline KPair link(OperatorPortPair source_port,
                                OperatorPortPair destination_port,
                                EdgeConfig edge_config = {}) {
  return KPair(std::move(source_port),
               std::move(destination_port),
               std::move(edge_config));
}

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_KPAIR_HPP_
