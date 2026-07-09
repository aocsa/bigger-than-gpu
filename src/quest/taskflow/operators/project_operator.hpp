// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_OPERATORS_PROJECT_OPERATOR_HPP_
#define QUEST_TASKFLOW_OPERATORS_PROJECT_OPERATOR_HPP_

/// @file project_operator.hpp
/// @brief Pipeline-role Project that evaluates pre-built physical expressions.

#include <cstdint>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/execution/batch_kernels.hpp"
#include "quest/physical_plan/expressions/physical_expr.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Streaming projection: evaluate exprs into a new RecordBatch.
class ProjectOperator : public SimpleOperator {
 public:
  ProjectOperator(std::uint32_t operator_id, datasource::Schema schema,
                  std::vector<ExpressionPtr> expressions)
      : SimpleOperator(operator_id, "Project", OperatorType::Project),
        schema_(std::move(schema)),
        expressions_(std::move(expressions)) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

 protected:
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override {
    auto host_result = execution::EnsureHostBlock(std::move(input));
    if (!host_result) {
      return std::unexpected(host_result.error());
    }
    auto host = std::move(*host_result);
    const auto* batch = host->record_batch();
    if (!batch) {
      return Status::Invalid("ProjectOperator: empty record batch");
    }

    auto projected = execution::ProjectRecordBatch(*batch, schema_, expressions_);
    if (!projected) {
      return std::unexpected(projected.error());
    }

    return std::make_shared<cache::HostMemoryBlock>(std::format("project_{}", id()),
                                                    std::move(*projected));
  }

 private:
  datasource::Schema schema_;
  std::vector<ExpressionPtr> expressions_;
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_OPERATORS_PROJECT_OPERATOR_HPP_
