#include "quest/taskflow/port.hpp"

#include "quest/taskflow/operator.hpp"

#include <format>

namespace quest::taskflow {

// =============================================================================
// Port Registration
// =============================================================================

void Port::RegisterPort(std::string port_name) {
  batch_holder_registry_[std::move(port_name)] = nullptr;
}

void Port::RegisterBatchHolder(
    const std::string& port_name,
    std::shared_ptr<cache::IBatchHolder> batch_holder) {
  batch_holder_registry_[port_name] = std::move(batch_holder);
}

// =============================================================================
// Port Access
// =============================================================================

Result<std::shared_ptr<cache::IBatchHolder>> Port::get(const std::string& port_name) const {
  auto iterator = find_port(port_name);
  if (iterator == batch_holder_registry_.end()) {
    return Error::NotFound(std::format("Port '{}' not found in operator {}",
                                       resolve_port_name(port_name), owner_operator_->id()));
  }
  return iterator->second;
}

// =============================================================================
// Lifecycle Management
// =============================================================================

void Port::Finish() {
  for (auto& [port_name, batch_holder] : batch_holder_registry_) {
    if (batch_holder) {
      batch_holder->Finish();
    }
  }
}

// =============================================================================
// Statistics
// =============================================================================

std::size_t Port::TotalBytesAdded() const {
  std::size_t total_bytes = 0;
  for (const auto& [port_name, batch_holder] : batch_holder_registry_) {
    if (batch_holder) {
      total_bytes += batch_holder->bytes_added();
    }
  }

  return total_bytes;
}

// =============================================================================
// Private Helpers
// =============================================================================

std::string Port::default_port_name() const {
  if (owner_operator_ != nullptr) {
    return std::to_string(owner_operator_->id());
  }
  return "";
}

std::string Port::resolve_port_name(const std::string& port_name) const {
  return port_name.empty() ? default_port_name() : port_name;
}

Port::RegistryMap::const_iterator Port::find_port(const std::string& port_name) const {
  return batch_holder_registry_.find(resolve_port_name(port_name));
}

}  // namespace quest::taskflow
