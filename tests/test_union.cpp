// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_union.cpp
/// @brief Phase 2.7 Stage F2: Union logical/operator + multi-input
///        named-port drain end-to-end.

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/execution/lowering/plan_to_graph.hpp"
#include "quest/plan/logical/logical_plan.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/union_operator.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::execution;
using namespace quest::columnar;
using namespace quest::testing;
using namespace quest::plan::logical;

namespace {

[[nodiscard]] std::filesystem::path EmployeesParquet() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "examples" /
         "employees_uncompressed.parquet";
}

[[nodiscard]] std::size_t TotalRows(const std::vector<RecordBatch>& batches) {
  std::size_t total = 0;
  for (const auto& batch : batches) {
    total += batch.num_rows();
  }
  return total;
}

/// @brief Finds the single pipeline whose sink is the given operator type.
[[nodiscard]] const Pipeline* FindPipelineBySinkType(const std::vector<PipelinePtr>& pipelines,
                                                     OperatorType type) {
  for (const auto& pipeline : pipelines) {
    if (pipeline->sink() != nullptr && pipeline->sink()->type() == type) {
      return pipeline.get();
    }
  }
  return nullptr;
}

/// @brief Flattens the single Int64 column of @p batches in emission order.
[[nodiscard]] std::vector<int64_t> Int64Column(const std::vector<RecordBatch>& batches) {
  std::vector<int64_t> values;
  for (const auto& batch : batches) {
    for (std::size_t i = 0; i < batch.num_rows(); ++i) {
      values.push_back(batch.column(0).Get<int64_t>(i));
    }
  }
  return values;
}

}  // namespace

// =============================================================================
// Logical Union + UnionCompatible
// =============================================================================

TEST(UnionPlanTest, LogicalSchemaChildrenToString) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan =
      QueryBuilder::FromParquet(EmployeesParquet().string())
          .value()
          .Select({"id"})
          .UnionAll(QueryBuilder::FromParquet(EmployeesParquet().string()).value().Select({"id"}))
          .BuildLogicalPlan();
  const auto* union_all = dynamic_cast<const Union*>(plan.get());
  ASSERT_NE(union_all, nullptr);
  EXPECT_EQ(union_all->schema().num_fields(), 1U);
  EXPECT_EQ(union_all->ToString(), "Union All");
  ASSERT_EQ(union_all->Children().size(), 2U);
}

TEST(UnionCompatibleTest, MatchingAndMismatched) {
  auto full = MakeTestRecordBatch(1).schema();
  EXPECT_TRUE(UnionCompatible(full, full));

  // Fewer fields → incompatible.
  Schema narrower({full.field(0)});
  if (full.num_fields() > 1) {
    EXPECT_FALSE(UnionCompatible(full, narrower));
  }
}

// =============================================================================
// UnionOperator unit
// =============================================================================

TEST(UnionOperatorTest, ContractAndPassthrough) {
  UnionOperator op{7};
  EXPECT_FALSE(op.IsBlocking());
  EXPECT_FALSE(op.IsSink());

  auto ports = op.DeclaredInputPorts();
  ASSERT_EQ(ports.size(), 2U);
  EXPECT_EQ(ports[0], "input_a");
  EXPECT_EQ(ports[1], "input_b");
  EXPECT_EQ(op.input_a().second, "input_a");
  EXPECT_EQ(op.input_b().second, "input_b");
  EXPECT_EQ(op.InputEdgeContract("input_a").barrier, BarrierType::Full);

  Operator& role = op;
  auto block = std::make_shared<cache::HostMemoryBlock>("in", MakeTestRecordBatch(4));
  ASSERT_OK_AND_ASSIGN(auto out, role.ProcessBatch(std::move(block)));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->num_rows(), 4U);
}

// =============================================================================
// Gate + shape
// =============================================================================

TEST(UnionE2ETest, CreateGraphBuildsMultiInputShape) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .UnionAll(QueryBuilder::FromParquet(EmployeesParquet().string()).value())
                  .BuildLogicalPlan();

  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  // Two scans + union + collector → four pipelines.
  ASSERT_EQ(pipelines.size(), 4U);

  const Pipeline* union_pipeline = FindPipelineBySinkType(pipelines, OperatorType::Union);
  ASSERT_NE(union_pipeline, nullptr);
  // Both input scans feed the union pipeline behind Full barriers.
  EXPECT_EQ(union_pipeline->FullBarrierProducers().size(), 2U);

  const Pipeline* collector_pipeline = FindPipelineBySinkType(pipelines, OperatorType::Output);
  ASSERT_NE(collector_pipeline, nullptr);
  ASSERT_EQ(collector_pipeline->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(collector_pipeline->FullBarrierProducers()[0], union_pipeline);
}

TEST(UnionE2ETest, SchemaMismatchRejectedByGate) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({"id"})
                  .UnionAll(QueryBuilder::FromParquet(EmployeesParquet().string()).value())
                  .BuildLogicalPlan();
  auto exec = CreateGraph(*plan);
  EXPECT_FALSE(exec.has_value());
}

// =============================================================================
// E2E execution
// =============================================================================

TEST(UnionE2ETest, GraphRowCountIsSumOfInputs) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  const auto path = EmployeesParquet().string();
  constexpr std::size_t kSingleRows = 100000;

  ASSERT_OK_AND_ASSIGN(auto graph_batches, QueryBuilder::FromParquet(path)
                                               .value()
                                               .UnionAll(QueryBuilder::FromParquet(path).value())
                                               .ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), 2 * kSingleRows);
}

TEST(UnionE2ETest, ConcatenatesLeftThenRight) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  const auto path = EmployeesParquet().string();

  ASSERT_OK_AND_ASSIGN(
      auto graph_batches,
      QueryBuilder::FromParquet(path)
          .value()
          .Select({"id"})
          .Limit(3)
          .UnionAll(QueryBuilder::FromParquet(path).value().Select({"id"}).Limit(5))
          .ExecuteOnGraph());
  EXPECT_EQ(Int64Column(graph_batches), (std::vector<int64_t>{1, 2, 3, 1, 2, 3, 4, 5}));
}

TEST(UnionE2ETest, SelectUnionReturnsEveryRowOfBothInputs) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  const auto path = EmployeesParquet().string();

  ASSERT_OK_AND_ASSIGN(auto graph_batches,
                       QueryBuilder::FromParquet(path)
                           .value()
                           .Select({"id"})
                           .UnionAll(QueryBuilder::FromParquet(path).value().Select({"id"}))
                           .ExecuteOnGraph());
  ASSERT_FALSE(graph_batches.empty());
  EXPECT_EQ(graph_batches.front().num_columns(), 1U);

  // The file's ids are 1..100000 in row order: expect that run twice.
  const auto ids = Int64Column(graph_batches);
  ASSERT_EQ(ids.size(), 200000U);
  for (std::size_t i = 0; i < ids.size(); ++i) {
    ASSERT_EQ(ids[i], static_cast<int64_t>(i % 100000) + 1) << "row " << i;
  }
}
