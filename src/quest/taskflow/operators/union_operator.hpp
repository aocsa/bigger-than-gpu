// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_UNION_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_UNION_OPERATOR_HPP_

/// @file union_operator.hpp
/// @brief Streaming UNION ALL: forwards two input streams as one.
///
/// The first real multi-input operator. It declares two named input ports
/// (`input_a`, `input_b`); the executor drains them in declared order (all of
/// input_a, then all of input_b), giving deterministic UNION ALL semantics.
/// The operator itself is a passthrough — the concatenation lives in the
/// executor's named-port claim path, mirroring the FOR_EACH lane drain.

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief UNION ALL of two input streams (passthrough; ports drained in order).
///
/// Not a Sink-accumulator: `IsBlocking()` is overridden to false so the
/// meta-pipeline treats it as a holder-fed multi-port consumer (one child
/// pipeline per input port behind a Full barrier) rather than a blocking
/// operator that flips from sink to source — the passthrough would otherwise
/// be placed in two pipelines and run twice.
class UnionOperator : public SimpleOperator {
 public:
  explicit UnionOperator(std::uint32_t operator_id)
      : SimpleOperator(operator_id, "Union", OperatorType::Union) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  /// @brief Streaming merge, not a blocking pipeline breaker (see class note).
  [[nodiscard]] bool IsBlocking() const noexcept override { return false; }

  /// @brief The two input ports, drained in declared order (input_a, input_b).
  [[nodiscard]] std::vector<std::string_view> DeclaredInputPorts() const override {
    return {"input_a", "input_b"};
  }

  /// @brief Both inputs are consumed behind a Full barrier: the producer
  ///        pipelines finish before this one drains their holders, so the
  ///        executor never parks a worker in a blocking Pull() waiting on a
  ///        producer that shares its worker pool.
  [[nodiscard]] EdgeConfig InputEdgeContract(std::string_view /*port*/) const override {
    EdgeConfig cfg;
    cfg.barrier = BarrierType::Full;
    return cfg;
  }

  /// @brief Typed port accessor for the left input (a typo is a compile error).
  [[nodiscard]] OperatorPortPair input_a() { return {this, "input_a"}; }

  [[nodiscard]] OperatorPortPair input_b() { return {this, "input_b"}; }

 protected:
  /// @brief Passthrough: forwards each claimed batch unchanged. The executor
  ///        supplies batches from input_a then input_b, so forwarding in
  ///        arrival order yields UNION ALL.
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override {
    return input;
  }
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_UNION_OPERATOR_HPP_
