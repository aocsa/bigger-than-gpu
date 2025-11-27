#include "quest/taskflow/port.hpp"

#include "quest/taskflow/operator.hpp"

#include <format>
#include <stdexcept>

namespace quest::taskflow {

// =============================================================================
// Port Registration
// =============================================================================

void Port::register_port(std::string port_name) {
  // Create an empty slot for this port name.
  // The batch holder will be assigned later when edges are created.
  batch_holder_registry_[std::move(port_name)] = nullptr;
}

void Port::register_batch_holder(
    const std::string& port_name,
    std::shared_ptr<cache::IBatchHolder> batch_holder) {
  // Associate the batch holder with the named port.
  batch_holder_registry_[port_name] = std::move(batch_holder);
}

// =============================================================================
// Port Access
// =============================================================================

std::shared_ptr<cache::IBatchHolder>& Port::get(const std::string& port_name) {
  // Resolve the port name (use default if empty).
  std::string resolved_name =
      port_name.empty() ? default_port_name() : port_name;

  // Look up the port in the registry.
  auto iterator = batch_holder_registry_.find(resolved_name);
  if (iterator == batch_holder_registry_.end()) {
    throw std::runtime_error(std::format(
        "Port '{}' not found in operator {}", resolved_name, owner_operator_->id()));
  }

  return iterator->second;
}

const std::shared_ptr<cache::IBatchHolder>& Port::get(
    const std::string& port_name) const {
  // Resolve the port name (use default if empty).
  std::string resolved_name =
      port_name.empty() ? default_port_name() : port_name;

  // Look up the port in the registry.
  auto iterator = batch_holder_registry_.find(resolved_name);
  if (iterator == batch_holder_registry_.end()) {
    throw std::runtime_error(std::format(
        "Port '{}' not found in operator {}", resolved_name, owner_operator_->id()));
  }

  return iterator->second;
}

// =============================================================================
// Lifecycle Management
// =============================================================================

void Port::finish() {
  // Signal completion on all batch holders.
  for (auto& [port_name, batch_holder] : batch_holder_registry_) {
    if (batch_holder) {
      batch_holder->finish();
    }
  }
}

bool Port::all_finished() const {
  // Check if all batch holders report finished status.
  for (const auto& [port_name, batch_holder] : batch_holder_registry_) {
    if (batch_holder && !batch_holder->is_finished()) {
      return false;
    }
  }
  return true;
}

bool Port::is_finished(const std::string& port_name) const {
  // Resolve the port name (use default if empty).
  std::string resolved_name =
      port_name.empty() ? default_port_name() : port_name;

  // Look up the port and check its status.
  auto iterator = batch_holder_registry_.find(resolved_name);
  if (iterator == batch_holder_registry_.end() || !iterator->second) {
    // Non-existent or empty ports are considered finished.
    return true;
  }

  return iterator->second->is_finished();
}

// =============================================================================
// Statistics
// =============================================================================

std::size_t Port::total_bytes_added() const {
  std::size_t total_bytes = 0;

  // Sum bytes across all batch holders.
  for (const auto& [port_name, batch_holder] : batch_holder_registry_) {
    if (batch_holder) {
      total_bytes += batch_holder->bytes_added();
    }
  }

  return total_bytes;
}

std::size_t Port::total_rows_added() const {
  std::size_t total_rows = 0;

  // Sum rows across all batch holders.
  for (const auto& [port_name, batch_holder] : batch_holder_registry_) {
    if (batch_holder) {
      total_rows += batch_holder->rows_added();
    }
  }

  return total_rows;
}

std::size_t Port::rows_added(const std::string& port_name) const {
  // Resolve the port name (use default if empty).
  std::string resolved_name =
      port_name.empty() ? default_port_name() : port_name;

  // Look up the port and return its row count.
  auto iterator = batch_holder_registry_.find(resolved_name);
  if (iterator == batch_holder_registry_.end() || !iterator->second) {
    return 0;
  }

  return iterator->second->rows_added();
}

// =============================================================================
// Private Helpers
// =============================================================================

std::string Port::default_port_name() const {
  // Use the owning operator's ID as the default port name.
  if (owner_operator_ != nullptr) {
    return std::to_string(owner_operator_->id());
  }
  return "";
}

}  // namespace quest::taskflow
