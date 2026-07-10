// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_limit.cpp
/// @brief Phase 2.7 Stage F1: TruncateRecordBatch, LimitOperator, LimitExec, e2e Limit.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/execution/batch_kernels.hpp"
#include "quest/execution/graph_builder.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/physical_plan.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/limit_operator.hpp"
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

/// @brief Stub physical source that emits a fixed list of batches.
class VectorSourceExec : public PhysicalPlan {
 public:
  VectorSourceExec(Schema schema, std::vector<RecordBatch> batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] std::vector<const PhysicalPlan*> Children() const override { return {}; }

  [[nodiscard]] Status Execute(const ResultCallback& callback) override {
    for (auto& batch : batches_) {
      callback(std::move(batch));
    }
    batches_.clear();
    return Status::OK();
  }

  [[nodiscard]] std::string ToString() const override { return "VectorSourceExec"; }

 private:
  Schema schema_;
  std::vector<RecordBatch> batches_;
};

}  // namespace

// =============================================================================
// TruncateRecordBatch
// =============================================================================

TEST(TruncateRecordBatchTest, ExactBoundary) {
  auto batch = MakeTestRecordBatch(5);
  ASSERT_OK_AND_ASSIGN(auto truncated, TruncateRecordBatch(batch, 5));
  EXPECT_EQ(truncated.num_rows(), 5U);
  EXPECT_EQ(std::get<int32_t>(truncated.column(0).GetValue(0)), 0);
  EXPECT_EQ(std::get<int32_t>(truncated.column(0).GetValue(4)), 4);
}

TEST(TruncateRecordBatchTest, MidBatch) {
  auto batch = MakeTestRecordBatch(10);
  ASSERT_OK_AND_ASSIGN(auto truncated, TruncateRecordBatch(batch, 3));
  EXPECT_EQ(truncated.num_rows(), 3U);
  EXPECT_EQ(std::get<int32_t>(truncated.column(0).GetValue(0)), 0);
  EXPECT_EQ(std::get<int32_t>(truncated.column(0).GetValue(2)), 2);
}

TEST(TruncateRecordBatchTest, ZeroRows) {
  auto batch = MakeTestRecordBatch(5);
  ASSERT_OK_AND_ASSIGN(auto truncated, TruncateRecordBatch(batch, 0));
  EXPECT_EQ(truncated.num_rows(), 0U);
  EXPECT_EQ(truncated.schema().num_fields(), batch.schema().num_fields());
}

TEST(TruncateRecordBatchTest, LimitExceedsRows) {
  auto batch = MakeTestRecordBatch(4);
  ASSERT_OK_AND_ASSIGN(auto truncated, TruncateRecordBatch(batch, 100));
  EXPECT_EQ(truncated.num_rows(), 4U);
}

// =============================================================================
// LimitOperator unit
// =============================================================================

TEST(LimitOperatorTest, ExactBoundaryPassesThrough) {
  LimitOperator op{1, 5};
  Operator& role = op;
  auto block = std::make_shared<cache::HostMemoryBlock>("in", MakeTestRecordBatch(5));
  ASSERT_OK_AND_ASSIGN(auto out, role.ProcessBatch(std::move(block)));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->num_rows(), 5U);
  EXPECT_EQ(op.remaining(), 0U);
}

TEST(LimitOperatorTest, TruncatesMidBatch) {
  LimitOperator op{1, 3};
  Operator& role = op;
  auto block = std::make_shared<cache::HostMemoryBlock>("in", MakeTestRecordBatch(10));
  ASSERT_OK_AND_ASSIGN(auto out, role.ProcessBatch(std::move(block)));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->num_rows(), 3U);
  EXPECT_EQ(op.remaining(), 0U);
}

TEST(LimitOperatorTest, LimitZeroEmitsEmpty) {
  LimitOperator op{1, 0};
  Operator& role = op;
  auto block = std::make_shared<cache::HostMemoryBlock>("in", MakeTestRecordBatch(5));
  ASSERT_OK_AND_ASSIGN(auto out, role.ProcessBatch(std::move(block)));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->num_rows(), 0U);
}

TEST(LimitOperatorTest, LimitExceedsRowsPassesThrough) {
  LimitOperator op{1, 100};
  Operator& role = op;
  auto block = std::make_shared<cache::HostMemoryBlock>("in", MakeTestRecordBatch(7));
  ASSERT_OK_AND_ASSIGN(auto out, role.ProcessBatch(std::move(block)));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->num_rows(), 7U);
  EXPECT_EQ(op.remaining(), 93U);
}

TEST(LimitOperatorTest, ExhaustedSubsequentBatchesEmpty) {
  LimitOperator op{1, 2};
  Operator& role = op;
  auto first = std::make_shared<cache::HostMemoryBlock>("a", MakeTestRecordBatch(5));
  ASSERT_OK_AND_ASSIGN(auto out1, role.ProcessBatch(std::move(first)));
  EXPECT_EQ(out1->num_rows(), 2U);

  auto second = std::make_shared<cache::HostMemoryBlock>("b", MakeTestRecordBatch(5));
  ASSERT_OK_AND_ASSIGN(auto out2, role.ProcessBatch(std::move(second)));
  EXPECT_EQ(out2->num_rows(), 0U);
  EXPECT_EQ(op.remaining(), 0U);
}

// =============================================================================
// Logical / physical
// =============================================================================

TEST(LimitPlanTest, LogicalSchemaAndToString) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({"id"})
                  .Limit(2)
                  .BuildLogicalPlan();
  const auto* limit = dynamic_cast<const Limit*>(plan.get());
  ASSERT_NE(limit, nullptr);
  EXPECT_EQ(limit->schema().num_fields(), 1U);
  EXPECT_EQ(limit->limit(), 2U);
  EXPECT_EQ(limit->ToString(), "Limit: 2");
  ASSERT_EQ(limit->Children().size(), 1U);
}

TEST(LimitPlanTest, LimitExecCollectsFirstN) {
  auto batch = MakeTestRecordBatch(10);
  auto input = std::make_unique<VectorSourceExec>(batch.schema(),
                                                  std::vector<RecordBatch>{batch});
  LimitExec limit(std::move(input), 4);

  std::size_t total = 0;
  ASSERT_OK(limit.Execute([&](RecordBatch b) { total += b.num_rows(); }));
  EXPECT_EQ(total, 4U);
  EXPECT_EQ(limit.ToString(), "LimitExec(4)");
}

TEST(LimitPlanTest, CreatePhysicalPlanBuildsLimitExec) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Limit(5)
                  .BuildLogicalPlan();
  ASSERT_OK_AND_ASSIGN(auto physical, CreatePhysicalPlan(*plan));
  EXPECT_NE(dynamic_cast<LimitExec*>(physical.get()), nullptr);
  EXPECT_EQ(physical->ToString(), "LimitExec(5)");
}

// =============================================================================
// E2E graph + shape
// =============================================================================

TEST(LimitE2ETest, CreateGraphFusesLimitIntoProducerPipeline) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({"id"})
                  .Limit(5)
                  .BuildLogicalPlan();

  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  ASSERT_EQ(pipelines.size(), 2U);
  const auto& producer = pipelines[0];
  const auto& collector = pipelines[1];

  // scan, project, limit — Limit is streaming and fuses before the Full barrier.
  EXPECT_EQ(producer->operators().size(), 3U);
  EXPECT_EQ(producer->operators().back()->type(), OperatorType::Limit);
  EXPECT_EQ(collector->operators().size(), 1U);
  ASSERT_EQ(collector->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(collector->FullBarrierProducers()[0], producer.get());
}

TEST(LimitE2ETest, GraphRowCountMatchesLimit) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();
  constexpr std::size_t kLimit = 3;

  ASSERT_OK_AND_ASSIGN(auto graph_batches, QueryBuilder::FromParquet(path)
                                               .value()
                                               .Select({"id"})
                                               .Limit(kLimit)
                                               .ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), kLimit);

  ASSERT_OK_AND_ASSIGN(auto callback_batches, QueryBuilder::FromParquet(path)
                                                  .value()
                                                  .Select({"id"})
                                                  .Limit(kLimit)
                                                  .ExecuteCollect());
  EXPECT_EQ(TotalRows(callback_batches), kLimit);
}

TEST(LimitE2ETest, LimitZero) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();
  ASSERT_OK_AND_ASSIGN(auto graph_batches,
                       QueryBuilder::FromParquet(path).value().Limit(0).ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), 0U);

  ASSERT_OK_AND_ASSIGN(auto callback_batches,
                       QueryBuilder::FromParquet(path).value().Limit(0).ExecuteCollect());
  EXPECT_EQ(TotalRows(callback_batches), 0U);
}

TEST(LimitE2ETest, LimitExceedsTotalRows) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();
  ASSERT_OK_AND_ASSIGN(auto all_batches, QueryBuilder::FromParquet(path).value().ExecuteCollect());
  const auto total = TotalRows(all_batches);

  ASSERT_OK_AND_ASSIGN(auto graph_batches, QueryBuilder::FromParquet(path)
                                               .value()
                                               .Limit(total + 100)
                                               .ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), total);
}
