// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_union.cpp
/// @brief Phase 2.7 Stage F2: Union logical/physical/operator + multi-input
///        named-port drain end-to-end.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/execution/graph_builder.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/physical_plan.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/union_operator.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::execution;
using namespace quest::datasource;
using namespace quest::testing;

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

/// @brief Stub physical source that emits a fixed list of batches (one-shot).
class VectorSourceExec : public PhysicalPlan {
 public:
  VectorSourceExec(Schema schema, std::vector<RecordBatch> batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }
  [[nodiscard]] std::vector<const PhysicalPlan*> Children() const override { return {}; }

  [[nodiscard]] Status Execute(const ResultCallback& callback) override {
    for (auto& batch : batches_) {
      if (!callback(std::move(batch))) {
        break;
      }
    }
    batches_.clear();
    return Status::OK();
  }

  [[nodiscard]] std::string ToString() const override { return "VectorSourceExec"; }

 private:
  Schema schema_;
  std::vector<RecordBatch> batches_;
};

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
// Physical UnionAllExec
// =============================================================================

TEST(UnionExecTest, ConcatenatesLeftThenRight) {
  auto left_batch = MakeTestRecordBatch(3);
  auto right_batch = MakeTestRecordBatch(5);
  auto left =
      std::make_unique<VectorSourceExec>(left_batch.schema(), std::vector<RecordBatch>{left_batch});
  auto right = std::make_unique<VectorSourceExec>(right_batch.schema(),
                                                  std::vector<RecordBatch>{right_batch});
  UnionAllExec union_all(std::move(left), std::move(right));

  std::vector<std::size_t> sizes;
  ASSERT_OK(union_all.Execute([&](RecordBatch b) {
    sizes.push_back(b.num_rows());
    return true;
  }));
  ASSERT_EQ(sizes.size(), 2U);
  EXPECT_EQ(sizes[0], 3U);  // left first
  EXPECT_EQ(sizes[1], 5U);  // then right
  EXPECT_EQ(union_all.ToString(), "UnionAllExec");
}

TEST(UnionExecTest, CreatePhysicalPlanBuildsUnionAllExec) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .UnionAll(QueryBuilder::FromParquet(EmployeesParquet().string()).value())
                  .BuildLogicalPlan();
  ASSERT_OK_AND_ASSIGN(auto physical, CreatePhysicalPlan(*plan));
  EXPECT_NE(dynamic_cast<UnionAllExec*>(physical.get()), nullptr);
}

TEST(UnionExecTest, SchemaMismatchRejectedByPhysicalPlan) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  // Left projects one column; right keeps the full schema → mismatch.
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({"id"})
                  .UnionAll(QueryBuilder::FromParquet(EmployeesParquet().string()).value())
                  .BuildLogicalPlan();
  auto physical = CreatePhysicalPlan(*plan);
  EXPECT_FALSE(physical.has_value());
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

  ASSERT_OK_AND_ASSIGN(auto one, QueryBuilder::FromParquet(path).value().ExecuteCollect());
  const auto single_rows = TotalRows(one);

  ASSERT_OK_AND_ASSIGN(auto graph_batches, QueryBuilder::FromParquet(path)
                                               .value()
                                               .UnionAll(QueryBuilder::FromParquet(path).value())
                                               .ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), 2 * single_rows);
}

TEST(UnionE2ETest, GraphParityWithCallbackEngine) {
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
  ASSERT_OK_AND_ASSIGN(auto callback_batches,
                       QueryBuilder::FromParquet(path)
                           .value()
                           .Select({"id"})
                           .UnionAll(QueryBuilder::FromParquet(path).value().Select({"id"}))
                           .ExecuteCollect());
  EXPECT_EQ(TotalRows(graph_batches), TotalRows(callback_batches));
}
