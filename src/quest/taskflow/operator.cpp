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

std::string Operator::MessageId() const {
  // Generate a unique identifier combining operator type and ID.
  // Format: "{type_as_int}_{operator_id}"
  return std::format("{}_{}", static_cast<int>(operator_type_), operator_id_);
}

std::pair<bool, std::uint64_t> Operator::GetEstimatedOutputNumRows() const {
  // Default estimation: assume 1:1 row ratio (output = input).

  // If all inputs are complete, return the actual count.
  if (InputAllFinished()) {
    return {true, TotalInputRowsAdded()};
  }

  // Try to get estimate from the owning graph.
  if (auto owning_graph = owning_graph_.lock()) {
    return owning_graph->EstimateInputRowCount(
        static_cast<std::int32_t>(operator_id_));
  }

  // No estimate available.
  return {false, 0};
}

// =============================================================================
// Pipeline Role Defaults
// =============================================================================

Result<cache::DataBlockPtr> Operator::ProcessBatch(cache::DataBlockPtr input) {
  // Default execute role: passthrough. Correct for operators whose work
  // happens in their source or sink role (scans, collectors).
  return input;
}

Result<std::optional<cache::DataBlockPtr>> Operator::GetNextBatch() {
  // Only sources (and finalized blocking operators) produce batches.
  return Status::Invalid(std::format(
      "Operator {} ({}) is not a source; GetNextBatch is not supported",
      operator_id_, to_string(operator_type_)));
}

Status Operator::Sink(cache::DataBlockPtr /*batch*/) {
  // Only sinks (blocking operators, collectors) may be fed final batches.
  return Status::Invalid(std::format(
      "Operator {} ({}) is not a sink; Sink is not supported",
      operator_id_, to_string(operator_type_)));
}

Status Operator::FinalizeSink() {
  // No-op for non-sink operators.
  return Status::OK();
}

std::shared_ptr<cache::IBatchHolder> Operator::InputCache() {
  // Use the operator's ID as the default port name.
  auto default_port_name = std::to_string(operator_id_);
  return input_port_.get(default_port_name);
}

std::shared_ptr<cache::IBatchHolder> Operator::OutputCache(
    const std::string& cache_id) {
  // Use the operator's ID if no specific cache is requested.
  std::string port_name =
      cache_id.empty() ? std::to_string(operator_id_) : cache_id;
  return output_port_.get(port_name);
}

Status Operator::AddToOutput(std::shared_ptr<cache::DataBlock> data,
                               const std::string& cache_id) {
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
  return batch_holder->Add(std::move(data));
}

void Operator::WaitIfOutputSaturated(const std::string& cache_id) {
  // Determine the port name for this output.
  std::string port_name =
      cache_id.empty() ? std::to_string(operator_id_) : cache_id;

  // Wait on the batch holder if it exists.
  auto& batch_holder = output_port_.get(port_name);
  if (batch_holder) {
    batch_holder->WaitIfSaturated();
  }
}

// =============================================================================
// SimpleOperator Implementation
// =============================================================================

ExecutionStatus SimpleOperator::run() {
  // Get the default input batch holder.
  auto input_batch_holder = InputCache();
  if (!input_batch_holder) {
    return ExecutionStatus::Stop;
  }

  // Process batches until the input is exhausted.
  while (true) {
    // Pull the next message from the input queue.
    auto message_result = input_batch_holder->Pull();

    if (!message_result.has_value()) {
      // Check if this is a normal end-of-stream signal.
      if (message_result.error().code() == ErrorCode::Invalid) {
        // Input exhausted - this is the normal completion path.
        break;
      }
      // Unexpected error - stop execution.
      return ExecutionStatus::Stop;
    }

    // Validate the data block.
    auto& data = message_result.value();
    if (!data) {
      // Skip empty data blocks.
      continue;
    }

    // Process the batch using the derived class implementation.
    auto processed_result = ProcessBatch(data);
    if (!processed_result.has_value()) {
      // Processing failed - stop execution.
      return ExecutionStatus::Stop;
    }

    // Send the processed data downstream.
    auto add_status = AddToOutput(std::move(processed_result.value()));
    if (!add_status.ok()) {
      // Failed to add to output - stop execution.
      return ExecutionStatus::Stop;
    }

    // Apply backpressure if downstream is saturated.
    WaitIfOutputSaturated();
  }

  return ExecutionStatus::Proceed;
}

}  // namespace quest::taskflow
