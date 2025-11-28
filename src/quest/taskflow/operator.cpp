#include "quest/taskflow/operator.hpp"

#include "quest/taskflow/graph.hpp"

#include <format>

namespace quest::taskflow {

// =============================================================================
// Operator Implementation
// =============================================================================

Operator::Operator(std::uint32_t operator_id,
                   std::string expression_string,
                   OperatorType operator_type)
    : operator_id_(operator_id),
      expression_string_(std::move(expression_string)),
      operator_type_(operator_type) {}

std::string Operator::message_id() const {
  // Generate a unique identifier combining operator type and ID.
  // Format: "{type_as_int}_{operator_id}"
  return std::format("{}_{}", static_cast<int>(operator_type_), operator_id_);
}

std::pair<bool, std::uint64_t> Operator::get_estimated_output_num_rows() const {
  // Default estimation: assume 1:1 row ratio (output = input).

  // If all inputs are complete, return the actual count.
  if (input_all_finished()) {
    return {true, total_input_rows_added()};
  }

  // Try to get estimate from the owning graph.
  if (auto owning_graph = owning_graph_.lock()) {
    return owning_graph->EstimateInputRowCount(
        static_cast<std::int32_t>(operator_id_));
  }

  // No estimate available.
  return {false, 0};
}

std::shared_ptr<cache::IBatchHolder> Operator::input_cache() {
  // Use the operator's ID as the default port name.
  auto default_port_name = std::to_string(operator_id_);
  return input_port_.get(default_port_name);
}

std::shared_ptr<cache::IBatchHolder> Operator::output_cache(
    const std::string& cache_id) {
  // Use the operator's ID if no specific cache is requested.
  std::string port_name =
      cache_id.empty() ? std::to_string(operator_id_) : cache_id;
  return output_port_.get(port_name);
}

Status Operator::add_to_output(std::shared_ptr<cache::CacheData> data,
                               const std::string& cache_id) {
  // Generate message ID for tracking this data through the pipeline.
  auto msg_id = message_id();
  if (!cache_id.empty()) {
    msg_id = cache_id + "_" + msg_id;
  }

  // Determine the port name for this output.
  std::string port_name =
      cache_id.empty() ? std::to_string(operator_id_) : cache_id;

  // Get the batch holder for this port.
  auto& batch_holder = output_port_.get(port_name);
  if (!batch_holder) {
    return Status::Invalid(std::format(
        "Output cache '{}' not found for operator {}", port_name, operator_id_));
  }

  // Add the data to the batch holder.
  return batch_holder->add(msg_id, std::move(data));
}

void Operator::wait_if_output_saturated(const std::string& cache_id) {
  // Determine the port name for this output.
  std::string port_name =
      cache_id.empty() ? std::to_string(operator_id_) : cache_id;

  // Wait on the batch holder if it exists.
  auto& batch_holder = output_port_.get(port_name);
  if (batch_holder) {
    batch_holder->wait_if_saturated();
  }
}

// =============================================================================
// SimpleOperator Implementation
// =============================================================================

ExecutionStatus SimpleOperator::run() {
  // Get the default input batch holder.
  auto input_batch_holder = input_cache();
  if (!input_batch_holder) {
    return ExecutionStatus::Stop;
  }

  // Process batches until the input is exhausted.
  while (true) {
    // Pull the next message from the input queue.
    auto message_result = input_batch_holder->pull();

    if (!message_result.has_value()) {
      // Check if this is a normal end-of-stream signal.
      if (message_result.error().code() == ErrorCode::Invalid) {
        // Input exhausted - this is the normal completion path.
        break;
      }
      // Unexpected error - stop execution.
      return ExecutionStatus::Stop;
    }

    // Validate the message contains data.
    auto& message = message_result.value();
    if (!message || !message->data()) {
      // Skip empty messages.
      continue;
    }

    // Process the batch using the derived class implementation.
    auto processed_result = process_batch(message->data());
    if (!processed_result.has_value()) {
      // Processing failed - stop execution.
      return ExecutionStatus::Stop;
    }

    // Send the processed data downstream.
    auto add_status = add_to_output(std::move(processed_result.value()));
    if (!add_status.ok()) {
      // Failed to add to output - stop execution.
      return ExecutionStatus::Stop;
    }

    // Apply backpressure if downstream is saturated.
    wait_if_output_saturated();
  }

  return ExecutionStatus::Proceed;
}

}  // namespace quest::taskflow
