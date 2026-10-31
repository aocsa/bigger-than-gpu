#include "quest/taskflow/operator.hpp"

#include <format>

#include "quest/taskflow/graph.hpp"

namespace quest::taskflow {

// =============================================================================
// Operator Implementation
// =============================================================================

Operator::Operator(std::uint32_t operator_id, std::string expression_string,
                   OperatorType operator_type)
    : operator_id_(operator_id),
      expression_string_(std::move(expression_string)),
      operator_type_(operator_type) {}

std::string Operator::MessageId() const {
  return std::format("{}_{}", static_cast<int>(operator_type_), operator_id_);
}

std::pair<bool, std::uint64_t> Operator::GetEstimatedOutputNumRows() const {
  // Default: assume 1:1 row ratio (output = input).
  if (InputAllFinished()) {
    return {true, TotalInputRowsAdded()};
  }
  if (auto owning_graph = owning_graph_.lock()) {
    return owning_graph->EstimateInputRowCount(static_cast<std::int32_t>(operator_id_));
  }
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
  return Status::Invalid(
      std::format("Operator {} ({}) is not a source; GetNextBatch is not supported", operator_id_,
                  to_string(operator_type_)));
}

Status Operator::Sink(cache::DataBlockPtr /*batch*/) {
  // Only sinks (blocking operators, collectors) may be fed final batches.
  return Status::Invalid(std::format("Operator {} ({}) is not a sink; Sink is not supported",
                                     operator_id_, to_string(operator_type_)));
}

Status Operator::FinalizeSink() {
  return Status::OK();
}

std::shared_ptr<cache::IBatchHolder> Operator::InputCache() {
  return input_port_.get(std::to_string(operator_id_));
}

Status Operator::AddToOutput(std::shared_ptr<cache::DataBlock> data, const std::string& cache_id) {
  std::string port_name = cache_id.empty() ? std::to_string(operator_id_) : cache_id;
  auto& batch_holder = output_port_.get(port_name);
  if (!batch_holder) {
    return Status::Invalid(
        std::format("Output cache '{}' not found for operator {}", port_name, operator_id_));
  }
  return batch_holder->Add(std::move(data));
}

}  // namespace quest::taskflow
