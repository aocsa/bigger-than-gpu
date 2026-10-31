// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_graph_bridge.cpp
/// @brief Phase 1: real operators + CreateGraph + ExecuteOnGraph + spill.

#include <gtest/gtest.h>

#include <filesystem>
#include <format>
#include <memory>
#include <string>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/cache/batch_holder.hpp"
#include "quest/execution/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;
using namespace quest::execution;
using namespace quest::datasource;

namespace {

[[nodiscard]] std::filesystem::path EmployeesParquet() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "examples" /
         "employees_uncompressed.parquet";
}


}  // namespace

TEST(GraphBridgeTest, CreateGraphBuildsFusedPipelinePlusCollectorPipeline) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Where(Col("salary") > Lit(50000.0))
                  .Select({"id", "name", "salary"})
                  .BuildLogicalPlan();

  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  // Streaming operators fuse; the collector sits behind a Full barrier in
  // its own pipeline (the RESULT_COLLECTOR shape) so final batches always
  // cross a spill-aware holder.
  ASSERT_EQ(pipelines.size(), 2U);
  const auto& producer = pipelines[0];
  const auto& collector = pipelines[1];

  EXPECT_EQ(producer->operators().size(), 3U);  // scan, filter, project
  EXPECT_TRUE(producer->source()->IsSource());

  EXPECT_EQ(collector->operators().size(), 1U);
  EXPECT_TRUE(collector->sink()->IsSink());
  ASSERT_EQ(collector->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(collector->FullBarrierProducers()[0], producer.get());
}

TEST(GraphBridgeTest, ParquetFilterProjectReturnsExpectedRows) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();

  ASSERT_OK_AND_ASSIGN(auto graph_batches, QueryBuilder::FromParquet(path)
                                               .value()
                                               .Where(Col("salary") > Lit(60000.0))
                                               .Select({"id", "name", "salary"})
                                               .ExecuteOnGraph());

  // Expected values computed independently from the Parquet file (DuckDB).
  std::size_t rows = 0;
  int64_t id_sum = 0;
  int64_t salary_sum = 0;
  int64_t prev_id = 0;
  for (const auto& batch : graph_batches) {
    ASSERT_EQ(batch.num_columns(), 3U);
    for (std::size_t i = 0; i < batch.num_rows(); ++i) {
      const auto id = std::get<int64_t>(batch.column(0).GetValue(i));
      const auto salary = std::get<int64_t>(batch.column(2).GetValue(i));
      EXPECT_GT(salary, 60000);
      EXPECT_GT(id, prev_id) << "rows must keep file order";
      prev_id = id;
      id_sum += id;
      salary_sum += salary;
      ++rows;
    }
  }
  EXPECT_EQ(rows, 86290U);
  EXPECT_EQ(id_sum, 4312145559);
  EXPECT_EQ(salary_sum, 13367835741);

  ASSERT_FALSE(graph_batches.empty());
  const auto& first = graph_batches.front();
  EXPECT_EQ(std::get<int64_t>(first.column(0).GetValue(0)), 2);
  EXPECT_EQ(std::get<std::string>(first.column(1).GetValue(0)), "Otapxhbqkl");
  EXPECT_EQ(std::get<int64_t>(first.column(2).GetValue(0)), 100169);
  const auto& last = graph_batches.back();
  const auto last_row = last.num_rows() - 1;
  EXPECT_EQ(std::get<int64_t>(last.column(0).GetValue(last_row)), 100000);
  EXPECT_EQ(std::get<std::string>(last.column(1).GetValue(last_row)), "Bpqguwmolp");
  EXPECT_EQ(std::get<int64_t>(last.column(2).GetValue(last_row)), 123265);
}

TEST(GraphBridgeTest, ArithmeticProjectionViaCreatePhysicalExpr) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  ASSERT_OK_AND_ASSIGN(auto batches, QueryBuilder::FromParquet(EmployeesParquet().string())
                                         .value()
                                         .Select({Col("id"), Col("salary") + Lit(1000.0)})
                                         .ExecuteOnGraph());

  ASSERT_FALSE(batches.empty());
  EXPECT_EQ(batches[0].num_columns(), 2U);
}

TEST(GraphBridgeTest, FlagshipSpillThroughRealOperators) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  auto cpu = std::make_shared<memory::CPUMemoryResource>(
      memory::CPUMemoryConfig{.max_bytes = 256, .saturation_threshold = 0.5});
  auto disk = std::make_shared<memory::DiskMemoryResource>();
  auto manager = std::make_shared<memory::MemoryResourceManager>(cpu, disk);

  // Pre-saturate like Stage 5: MaybeSpill only fires when should_spill().
  std::vector<std::shared_ptr<HostMemoryBlock>> keep_alive;
  for (int i = 0; i < 8; ++i) {
    keep_alive.push_back(std::make_shared<HostMemoryBlock>(
        std::format("pad_{}", i), quest::testing::MakeTestRecordBatch(10), cpu));
  }
  ASSERT_TRUE(cpu->is_saturated());

  // THE flagship: a real query, through the real gate (CreateGraph), with
  // filter+project in the path — the blocks that reach the collector's
  // Full-barrier holder are ProjectOperator outputs, so this proves both
  // that the collector pipeline break exists AND that intermediate
  // operators stamp memory resources onto their outputs (without which
  // ToDisk() fails and MaybeSpill silently keeps everything in RAM).
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Where(Col("salary") > Lit(50000.0))
                  .Select({"id", "name", "salary"})
                  .BuildLogicalPlan();

  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan, manager));
  ASSERT_OK(exec->Execute());

  const auto& collector = exec->collector();
  EXPECT_GT(collector.total_rows(), 0U);
  EXPECT_GT(collector.disk_tier_count(), 0U)
      << "tiny CPU budget must spill at least one batch through the "
         "collector's Full-barrier holder";

  // Spilled blocks must still materialize correctly (disk -> CPU upgrade).
  ASSERT_OK_AND_ASSIGN(auto batches, collector.CollectBatches());
  std::size_t rows = 0;
  for (const auto& batch : batches) {
    rows += batch.num_rows();
  }
  EXPECT_EQ(rows, collector.total_rows());
}

TEST(ConcatenatingBatchHolderTest, MultipleBlocksConcatenate) {
  ConcatenatingBatchHolder holder(1);
  ASSERT_OK(
      holder.Add(std::make_shared<HostMemoryBlock>("a", quest::testing::MakeTestRecordBatch(3))));
  ASSERT_OK(
      holder.Add(std::make_shared<HostMemoryBlock>("b", quest::testing::MakeTestRecordBatch(2))));
  holder.Finish();

  ASSERT_OK_AND_ASSIGN(auto merged, holder.Pull());
  ASSERT_TRUE(merged.has_value());
  EXPECT_EQ((*merged)->num_rows(), 5U);
}
