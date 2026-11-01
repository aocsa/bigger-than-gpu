// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file project_operator.hpp
/// @brief Pipeline-role Project that evaluates pre-built physical expressions.

#include <cstdint>
#include <format>
#include <memory>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/common/result.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Streaming projection: evaluate exprs into a new RecordBatch.
class ProjectOperator : public SimpleOperator {
 public:
  /// @param memory_manager Stamped onto output blocks so they stay visible
  ///        to saturation accounting and spillable at pipeline breaks.
  ProjectOperator(std::uint32_t operator_id, columnar::Schema schema,
                  std::vector<plan::physical::ExpressionPtr> expressions,
                  std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr)
      : SimpleOperator(operator_id, "Project", OperatorType::Project),
        schema_(std::move(schema)),
        expressions_(std::move(expressions)),
        memory_manager_(std::move(memory_manager)) {}

 protected:
  [[nodiscard]] Result<cache::DataBlockPtr> ProcessBatch(cache::DataBlockPtr input) override {
    // `host` stays alive until the output is tracked: memory accounting sees both at once.
    auto host = cache::EnsureHostBlock(std::move(input));
    if (!host) {
      return std::unexpected(host.error());
    }
    auto projected = execution::ProjectRecordBatch(*(*host)->record_batch(), schema_, expressions_);
    if (!projected) {
      return std::unexpected(projected.error());
    }
    return cache::MakeTrackedHostBlock(std::format("project_{}", id()), std::move(*projected),
                                       memory_manager_);
  }

 private:
  columnar::Schema schema_;
  std::vector<plan::physical::ExpressionPtr> expressions_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;
};

}  // namespace quest::taskflow
