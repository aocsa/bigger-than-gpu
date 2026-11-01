// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_limit.cpp
/// @brief Phase 2.7 Stage F1: TruncateRecordBatch, LimitOperator, e2e Limit.

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"
#include "quest/execution/lowering/plan_to_graph.hpp"
#include "quest/plan/logical/logical_plan.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/limit_operator.hpp"
#include "quest/taskflow/pipeline.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "support/test_utils.hpp"

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

/// @brief Graph source that counts GetNextBatch calls.
class CountingVectorSource : public Operator {
 public:
  CountingVectorSource(std::uint32_t id, std::size_t batch_count, std::size_t rows_per_batch)
      : Operator(id, "counting_vector_source", OperatorType::TableScan),
        remaining_(batch_count),
        rows_per_batch_(rows_per_batch) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  [[nodiscard]] std::size_t get_next_batch_calls() const noexcept {
    return get_next_batch_calls_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override {
    get_next_batch_calls_.fetch_add(1, std::memory_order_relaxed);
    if (remaining_ == 0) {
      return std::optional<cache::DataBlockPtr>{std::nullopt};
    }
    --remaining_;
    return std::optional<cache::DataBlockPtr>{std::make_shared<cache::HostMemoryBlock>(
        "b" + std::to_string(remaining_), MakeTestRecordBatch(rows_per_batch_))};
  }

 private:
  std::size_t remaining_;
  std::size_t rows_per_batch_;
  std::atomic<std::size_t> get_next_batch_calls_{0};
};

/// @brief Collecting sink for pipeline early-stop tests.
class CollectingSink : public Operator {
 public:
  explicit CollectingSink(std::uint32_t id)
      : Operator(id, "collecting_sink", OperatorType::Filter) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(cache::DataBlockPtr batch) override {
    total_rows_ += batch->num_rows();
    ++batch_count_;
    return {};
  }

  [[nodiscard]] std::size_t total_rows() const noexcept { return total_rows_; }
  [[nodiscard]] std::size_t batch_count() const noexcept { return batch_count_; }

 private:
  std::size_t total_rows_{0};
  std::size_t batch_count_{0};
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
  EXPECT_TRUE(op.ShouldStopSource());
}

TEST(LimitOperatorTest, TruncatesMidBatch) {
  LimitOperator op{1, 3};
  Operator& role = op;
  auto block = std::make_shared<cache::HostMemoryBlock>("in", MakeTestRecordBatch(10));
  ASSERT_OK_AND_ASSIGN(auto out, role.ProcessBatch(std::move(block)));
  ASSERT_NE(out, nullptr);
  EXPECT_EQ(out->num_rows(), 3U);
  EXPECT_EQ(op.remaining(), 0U);
  EXPECT_TRUE(op.ShouldStopSource());
}

TEST(LimitOperatorTest, LimitZeroEmitsEmpty) {
  LimitOperator op{1, 0};
  Operator& role = op;
  EXPECT_TRUE(op.ShouldStopSource());
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
  EXPECT_FALSE(op.ShouldStopSource());
}

TEST(LimitOperatorTest, ExhaustedSubsequentBatchesEmpty) {
  LimitOperator op{1, 2};
  Operator& role = op;
  auto first = std::make_shared<cache::HostMemoryBlock>("a", MakeTestRecordBatch(5));
  ASSERT_OK_AND_ASSIGN(auto out1, role.ProcessBatch(std::move(first)));
  EXPECT_EQ(out1->num_rows(), 2U);
  EXPECT_TRUE(op.ShouldStopSource());

  auto second = std::make_shared<cache::HostMemoryBlock>("b", MakeTestRecordBatch(5));
  ASSERT_OK_AND_ASSIGN(auto out2, role.ProcessBatch(std::move(second)));
  EXPECT_EQ(out2->num_rows(), 0U);
  EXPECT_EQ(op.remaining(), 0U);
}

// =============================================================================
// Logical plan
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

// =============================================================================
// Graph early-stop (ShouldStopSource)
// =============================================================================

TEST(LimitOperatorTest, PipelineStopsClaimingAfterLimit) {
  // 10 batches of 3 rows available; Limit(4) should claim only 2 source batches
  // (+ possibly one EOS check is not done because we break before next claim).
  CountingVectorSource source{0, /*batch_count=*/10, /*rows_per_batch=*/3};
  LimitOperator limit{1, 4};
  CollectingSink sink{2};

  auto pipeline = std::make_unique<Pipeline>(0);
  pipeline->PrependOperator(&sink);
  pipeline->PrependOperator(&limit);
  pipeline->PrependOperator(&source);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(pipeline));

  PipelineExecutor executor{1};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  EXPECT_EQ(sink.total_rows(), 4U);
  EXPECT_EQ(limit.remaining(), 0U);
  EXPECT_TRUE(limit.ShouldStopSource());
  // Two successful claims (3+1 rows), then ShouldStopSource breaks before a
  // third GetNextBatch. No EOS claim.
  EXPECT_EQ(source.get_next_batch_calls(), 2U);
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

  // Select({"id"}) on the scan folds into a BindableTableScan (F3), so the
  // producer is [bindable_scan, limit] — no separate ProjectOperator. Limit is
  // streaming and fuses before the Full barrier.
  EXPECT_EQ(producer->operators().size(), 2U);
  EXPECT_EQ(producer->operators().front()->type(), OperatorType::BindableTableScan);
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

  ASSERT_OK_AND_ASSIGN(
      auto graph_batches,
      QueryBuilder::FromParquet(path).value().Select({"id"}).Limit(kLimit).ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), kLimit);

  // The file's ids are 1..100000 in row order; Limit keeps the first rows.
  std::vector<int64_t> ids;
  for (const auto& batch : graph_batches) {
    ASSERT_EQ(batch.num_columns(), 1U);
    for (std::size_t i = 0; i < batch.num_rows(); ++i) {
      ids.push_back(std::get<int64_t>(batch.column(0).GetValue(i)));
    }
  }
  EXPECT_EQ(ids, (std::vector<int64_t>{1, 2, 3}));
}

TEST(LimitE2ETest, LimitZero) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();
  ASSERT_OK_AND_ASSIGN(auto graph_batches,
                       QueryBuilder::FromParquet(path).value().Limit(0).ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), 0U);
}

TEST(LimitE2ETest, LimitExceedsTotalRows) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();
  constexpr std::size_t kTotalRows = 100000;

  ASSERT_OK_AND_ASSIGN(
      auto graph_batches,
      QueryBuilder::FromParquet(path).value().Limit(kTotalRows + 100).ExecuteOnGraph());
  EXPECT_EQ(TotalRows(graph_batches), kTotalRows);
}
