#pragma once

/// @file port.hpp
/// @brief Port class for managing operator input/output connections.
///
/// A Port represents a connection point on an Operator where data flows in
/// (input port) or out (output port). Each port can have multiple named
/// connections, allowing operators like joins to have distinct input streams.
///
/// Port naming conventions:
/// - Simple operators: Use the operator's ID as the port name
/// - Multi-input operators: Use descriptive names like "left_input", "right_input"
/// - Partitioned outputs: Use indexed names like "output_0", "output_1"
///
/// Example usage:
/// @code
///   // Single input/output (most operators)
///   auto data = input_port.get("42");  // Get port for operator ID 42
///
///   // Multi-input operator (like a join)
///   input_port.register_port("left_input");
///   input_port.register_port("right_input");
///   auto left = input_port.get("left_input");
///   auto right = input_port.get("right_input");
/// @endcode

#include <map>
#include <memory>
#include <string>

#include "quest/cache/batch_holder.hpp"

namespace quest::taskflow {

// Forward declaration
class Operator;

/// @brief Manages connections between operators via BatchHolders.
///
/// A port maintains a collection of named batch holder connections. For simple
/// operators, there's typically one connection using the operator's ID as the
/// name. For multi-input operators (like joins), there are multiple named
/// connections representing different input streams.
///
/// Thread safety: Ports are not thread-safe. They are typically modified only
/// during graph construction (single-threaded) and read during execution.
class Port {
 public:
  /// @brief Constructs a port associated with an operator.
  ///
  /// @param owner_operator The operator that owns this port.
  explicit Port(Operator* owner_operator) noexcept
      : owner_operator_(owner_operator) {}

  ~Port() = default;

  // Copyable for operator move semantics.
  Port(const Port&) = default;
  Port& operator=(const Port&) = default;

  // Movable for efficient transfers.
  Port(Port&&) noexcept = default;
  Port& operator=(Port&&) noexcept = default;

  // ===========================================================================
  // Port Registration
  // ===========================================================================

  /// @brief Registers a named port slot.
  ///
  /// Creates an empty slot that can later be populated with a batch holder.
  /// Used during graph construction to declare expected connections.
  ///
  /// @param port_name The unique name for this port.
  void register_port(std::string port_name);

  /// @brief Associates a batch holder with a named port.
  ///
  /// Connects a batch holder to a port, enabling data flow. Called by the
  /// graph when creating edges between operators.
  ///
  /// @param port_name The name of the port to connect.
  /// @param batch_holder The batch holder for data transfer.
  void register_batch_holder(const std::string& port_name,
                             std::shared_ptr<cache::IBatchHolder> batch_holder);

  // ===========================================================================
  // Port Access
  // ===========================================================================

  /// @brief Gets the batch holder for a named port.
  ///
  /// If an empty port name is provided, returns the batch holder for the
  /// default port (named after the owning operator's ID).
  ///
  /// @param port_name The port name (empty for default).
  /// @return Reference to the batch holder shared_ptr.
  /// @throws std::runtime_error if the port doesn't exist.
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder>& get(
      const std::string& port_name = "");

  /// @brief Gets the batch holder for a named port (const version).
  [[nodiscard]] const std::shared_ptr<cache::IBatchHolder>& get(
      const std::string& port_name = "") const;

  /// @brief Array subscript access to batch holders.
  ///
  /// Unlike get(), this creates a new entry if the port doesn't exist.
  ///
  /// @param port_name The port name.
  /// @return Reference to the batch holder shared_ptr.
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder>& operator[](
      const std::string& port_name) {
    return batch_holder_registry_[port_name];
  }

  // ===========================================================================
  // Port Queries
  // ===========================================================================

  /// @brief Returns the number of registered ports.
  [[nodiscard]] std::size_t count() const noexcept {
    return batch_holder_registry_.size();
  }

  /// @brief Checks if a port with the given name exists.
  [[nodiscard]] bool has_port(const std::string& port_name) const {
    return batch_holder_registry_.contains(port_name);
  }

  /// @brief Gets the operator that owns this port.
  [[nodiscard]] Operator* owner() const noexcept { return owner_operator_; }

  // ===========================================================================
  // Lifecycle Management
  // ===========================================================================

  /// @brief Marks all batch holders as finished.
  ///
  /// Called when the owning operator completes execution to signal to
  /// downstream operators that no more data will be sent.
  void finish();

  /// @brief Checks if all batch holders are finished.
  ///
  /// @return true if all ports are finished or have no batch holder.
  [[nodiscard]] bool all_finished() const;

  /// @brief Checks if a specific port's batch holder is finished.
  ///
  /// @param port_name The port name (empty for default).
  /// @return true if finished or non-existent.
  [[nodiscard]] bool is_finished(const std::string& port_name = "") const;

  // ===========================================================================
  // Statistics
  // ===========================================================================

  /// @brief Gets total bytes added across all ports.
  [[nodiscard]] std::size_t total_bytes_added() const;

  /// @brief Gets total rows added across all ports.
  [[nodiscard]] std::size_t total_rows_added() const;

  /// @brief Gets rows added to a specific port.
  ///
  /// @param port_name The port name (empty for default).
  [[nodiscard]] std::size_t rows_added(const std::string& port_name = "") const;

  // ===========================================================================
  // Iteration Support
  // ===========================================================================

  /// @brief Returns iterator to the beginning of the port registry.
  [[nodiscard]] auto begin() { return batch_holder_registry_.begin(); }
  [[nodiscard]] auto end() { return batch_holder_registry_.end(); }
  [[nodiscard]] auto begin() const { return batch_holder_registry_.begin(); }
  [[nodiscard]] auto end() const { return batch_holder_registry_.end(); }

 private:
  // Type alias for the registry map
  using RegistryMap = std::map<std::string, std::shared_ptr<cache::IBatchHolder>>;

  /// @brief Gets the default port name based on the owning operator's ID.
  [[nodiscard]] std::string default_port_name() const;

  /// @brief Resolves the port name (uses default if empty) and returns the name.
  [[nodiscard]] std::string resolve_port_name(const std::string& port_name) const;

  /// @brief Looks up a port and returns an iterator. Throws if not found.
  [[nodiscard]] RegistryMap::iterator find_port_or_throw(const std::string& port_name);

  /// @brief Looks up a port and returns a const iterator. Throws if not found.
  [[nodiscard]] RegistryMap::const_iterator find_port_or_throw(const std::string& port_name) const;

  /// @brief Looks up a port and returns an iterator (no throw, may be end()).
  [[nodiscard]] RegistryMap::const_iterator find_port(const std::string& port_name) const;

  // The operator that owns this port.
  Operator* owner_operator_;

  // Registry mapping port names to batch holders.
  RegistryMap batch_holder_registry_;
};

// =============================================================================
// Variadic Port Registration Helper
// =============================================================================

namespace detail {

/// @brief Recursive helper for registering multiple ports.
template <typename PortType, typename FirstName, typename... RestNames>
void register_ports_impl(PortType& port,
                         FirstName&& first,
                         RestNames&&... rest) {
  port.register_port(std::forward<FirstName>(first));
  if constexpr (sizeof...(rest) > 0) {
    register_ports_impl(port, std::forward<RestNames>(rest)...);
  }
}

}  // namespace detail

/// @brief Registers multiple ports at once using variadic arguments.
///
/// Convenience function for multi-input operators to register all their
/// input ports in a single call.
///
/// @code
///   add_ports(input_port, "left_input", "right_input", "probe_input");
/// @endcode
///
/// @param port The port to register names on.
/// @param names The port names to register.
template <typename... PortNames>
void add_ports(Port& port, PortNames&&... names) {
  if constexpr (sizeof...(names) > 0) {
    detail::register_ports_impl(port, std::forward<PortNames>(names)...);
  }
}

}  // namespace quest::taskflow
