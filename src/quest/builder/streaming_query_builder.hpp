// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_STREAMING_QUERY_BUILDER_HPP
#define QUEST_STREAMING_QUERY_BUILDER_HPP

/// @file streaming_query_builder.hpp
/// @brief Streaming query builder with pull-based coroutine execution and backpressure.
///
/// Provides lazy, on-demand batch generation via C++20 coroutines.
/// The consumer controls the pace of execution, providing natural backpressure.
///
/// For synchronous push-based execution, see query_builder.hpp.

#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/builder/generator.hpp"
#include "quest/builder/query_plan.hpp"

namespace quest {

// =============================================================================
// Streaming Query Builder (Coroutine / Pull-based Execution)
// =============================================================================

/// @brief Streaming query builder with backpressure via C++20 coroutines.
///
/// Example:
/// @code
///   auto result = StreamingQueryBuilder::FromParquet("large_file.parquet");
///   if (!result) { /* handle error */ }
///
///   // Consumer controls the pace - only one batch in memory at a time
///   for (auto batch_result : std::move(result.value())
///       .Where(Col("salary") > Lit(50000))
///       .Select({"id", "name"})
///       .ExecuteStream()) {
///     if (!batch_result) { break; }
///     process(std::move(*batch_result));
///   }
/// @endcode
class StreamingQueryBuilder {
 public:
  // Rule of Five: Move-only (plan ownership)
  StreamingQueryBuilder(StreamingQueryBuilder&&) noexcept = default;
  StreamingQueryBuilder& operator=(StreamingQueryBuilder&&) noexcept = default;
  StreamingQueryBuilder(const StreamingQueryBuilder&) = delete;
  StreamingQueryBuilder& operator=(const StreamingQueryBuilder&) = delete;
  ~StreamingQueryBuilder() = default;

  // ===========================================================================
  // Factory Methods
  // ===========================================================================

  [[nodiscard]] static Result<StreamingQueryBuilder> FromParquet(
      const std::string& filename) {
    auto plan_result = QueryPlan::FromParquet(filename);
    if (!plan_result) {
      return std::unexpected(plan_result.error());
    }
    return StreamingQueryBuilder(std::move(*plan_result));
  }

  [[nodiscard]] static StreamingQueryBuilder From(
      std::shared_ptr<DataSource> source, const std::string& name) {
    return StreamingQueryBuilder(QueryPlan::From(std::move(source), name));
  }

  // ===========================================================================
  // Query Building (Fluent Interface)
  // ===========================================================================

  [[nodiscard]] StreamingQueryBuilder Where(const ExprBuilder& condition) && {
    return StreamingQueryBuilder(std::move(plan_).Where(condition));
  }

  [[nodiscard]] StreamingQueryBuilder Select(
      std::initializer_list<std::string> columns) && {
    return StreamingQueryBuilder(std::move(plan_).Select(columns));
  }

  [[nodiscard]] StreamingQueryBuilder Select(
      std::initializer_list<ExprBuilder> exprs) && {
    return StreamingQueryBuilder(std::move(plan_).Select(exprs));
  }

  [[nodiscard]] StreamingQueryBuilder Select(
      const std::vector<ExprBuilder>& exprs) && {
    return StreamingQueryBuilder(std::move(plan_).Select(exprs));
  }

  [[nodiscard]] StreamingQueryBuilder Limit(std::size_t n) && {
    return StreamingQueryBuilder(std::move(plan_).Limit(n));
  }

  // ===========================================================================
  // Plan Building
  // ===========================================================================

  [[nodiscard]] LogicalPlanPtr BuildLogicalPlan() && {
    return std::move(plan_).BuildLogicalPlan();
  }

  [[nodiscard]] Result<PhysicalPlanPtr> BuildPhysicalPlan() && {
    return std::move(plan_).BuildPhysicalPlan();
  }

  // ===========================================================================
  // Execution Methods (Streaming / Pull-based)
  // ===========================================================================

  /// Lazy pull-based execution. Current impl collects all batches first — true
  /// streaming requires an async PhysicalPlan::Execute.
  [[nodiscard]] BatchGenerator ExecuteStream() && {
    auto physical_result = CreatePhysicalPlan(plan_.plan());
    if (!physical_result) {
      co_yield std::unexpected(physical_result.error());
      co_return;
    }

    auto physical_plan = std::move(*physical_result);

    // Current PhysicalPlan::Execute is synchronous, so we collect first.
    // For true backpressure, the execution model would need to be coroutine-based.
    std::vector<datasource::RecordBatch> batches;
    auto status = physical_plan->Execute(
        [&batches](datasource::RecordBatch batch) {
          batches.push_back(std::move(batch));
          return true;
        });

    if (!status) {
      co_yield std::unexpected(status.error());
      co_return;
    }

    for (auto& batch : batches) {
      co_yield std::move(batch);
    }
  }

  // ===========================================================================
  // Utility Methods
  // ===========================================================================

  [[nodiscard]] std::string ToString() const { return plan_.ToString(); }

  [[nodiscard]] explicit operator bool() const noexcept {
    return static_cast<bool>(plan_);
  }

  [[nodiscard]] datasource::Schema schema() const { return plan_.schema(); }

 private:
  explicit StreamingQueryBuilder(QueryPlan plan) noexcept
      : plan_(std::move(plan)) {}

  QueryPlan plan_;
};

}  // namespace quest

#endif  // QUEST_STREAMING_QUERY_BUILDER_HPP
