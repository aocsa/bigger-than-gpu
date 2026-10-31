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
  explicit Port(Operator* owner_operator) noexcept
      : owner_operator_(owner_operator) {}

  ~Port() = default;

  Port(const Port&) = default;
  Port& operator=(const Port&) = default;

  Port(Port&&) noexcept = default;
  Port& operator=(Port&&) noexcept = default;

  // ===========================================================================
  // Port Registration
  // ===========================================================================

  /// @brief Declares a named port slot (filled by RegisterBatchHolder when an edge is created).
  void RegisterPort(std::string port_name);

  void RegisterBatchHolder(const std::string& port_name,
                           std::shared_ptr<cache::IBatchHolder> batch_holder);

  // ===========================================================================
  // Port Access
  // ===========================================================================

  /// @brief Returns the holder for the named port; empty string resolves to the operator-ID port.
  /// @throws std::runtime_error if the port doesn't exist.
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder>& get(
      const std::string& port_name = "");

  [[nodiscard]] const std::shared_ptr<cache::IBatchHolder>& get(
      const std::string& port_name = "") const;

  /// @brief Unlike get(), inserts a new entry if the port doesn't exist.
  [[nodiscard]] std::shared_ptr<cache::IBatchHolder>& operator[](
      const std::string& port_name) {
    return batch_holder_registry_[port_name];
  }

  // ===========================================================================
  // Port Queries
  // ===========================================================================

  [[nodiscard]] std::size_t count() const noexcept {
    return batch_holder_registry_.size();
  }

  [[nodiscard]] bool has_port(const std::string& port_name) const {
    return batch_holder_registry_.contains(port_name);
  }

  // ===========================================================================
  // Lifecycle Management
  // ===========================================================================

  /// @brief Signals all batch holders that no more data will arrive.
  void Finish();

  // ===========================================================================
  // Statistics
  // ===========================================================================

  [[nodiscard]] std::size_t TotalBytesAdded() const;

  // ===========================================================================
  // Iteration Support
  // ===========================================================================

  [[nodiscard]] auto begin() { return batch_holder_registry_.begin(); }
  [[nodiscard]] auto end() { return batch_holder_registry_.end(); }
  [[nodiscard]] auto begin() const { return batch_holder_registry_.begin(); }
  [[nodiscard]] auto end() const { return batch_holder_registry_.end(); }

 private:
  using RegistryMap = std::map<std::string, std::shared_ptr<cache::IBatchHolder>>;

  [[nodiscard]] std::string default_port_name() const;
  [[nodiscard]] std::string resolve_port_name(const std::string& port_name) const;
  [[nodiscard]] RegistryMap::iterator find_port_or_throw(const std::string& port_name);
  [[nodiscard]] RegistryMap::const_iterator find_port_or_throw(const std::string& port_name) const;
  [[nodiscard]] RegistryMap::const_iterator find_port(const std::string& port_name) const;

  Operator* owner_operator_;
  RegistryMap batch_holder_registry_;
};

// =============================================================================
// Variadic Port Registration Helper
// =============================================================================

namespace detail {

/// @brief Recursive helper for registering multiple ports.
template <typename PortType, typename FirstName, typename... RestNames>
void RegisterPortsImpl(PortType& port,
                       FirstName&& first,
                       RestNames&&... rest) {
  port.RegisterPort(std::forward<FirstName>(first));
  if constexpr (sizeof...(rest) > 0) {
    RegisterPortsImpl(port, std::forward<RestNames>(rest)...);
  }
}

}  // namespace detail

/// @brief Registers multiple ports in one call.
/// @code
///   AddPorts(input_port, "left_input", "right_input", "probe_input");
/// @endcode
template <typename... PortNames>
void AddPorts(Port& port, PortNames&&... names) {
  if constexpr (sizeof...(names) > 0) {
    detail::RegisterPortsImpl(port, std::forward<PortNames>(names)...);
  }
}

}  // namespace quest::taskflow
