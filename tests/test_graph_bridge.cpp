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

[[nodiscard]] std::string BatchFingerprint(const RecordBatch& batch) {
  return batch.ToCSV();
}

}  // namespace

TEST(GraphBridgeTest, CreateGraphBuildsSingleFusedPipeline) {
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
  ASSERT_EQ(pipelines.size(), 1U);
  // scan, filter, project, collector
  EXPECT_EQ(pipelines[0]->operators().size(), 4U);
  EXPECT_TRUE(pipelines[0]->source()->IsSource());
  EXPECT_TRUE(pipelines[0]->sink()->IsSink());
}

TEST(GraphBridgeTest, ParquetQueryParityCallbackVsGraph) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }

  const auto path = EmployeesParquet().string();

  ASSERT_OK_AND_ASSIGN(auto callback_batches, QueryBuilder::FromParquet(path)
                                                  .value()
                                                  .Where(Col("salary") > Lit(60000.0))
                                                  .Select({"id", "name", "salary"})
                                                  .ExecuteCollect());

  ASSERT_OK_AND_ASSIGN(auto graph_batches, QueryBuilder::FromParquet(path)
                                               .value()
                                               .Where(Col("salary") > Lit(60000.0))
                                               .Select({"id", "name", "salary"})
                                               .ExecuteOnGraph());

  ASSERT_EQ(callback_batches.size(), graph_batches.size());
  for (size_t i = 0; i < callback_batches.size(); ++i) {
    EXPECT_EQ(BatchFingerprint(callback_batches[i]), BatchFingerprint(graph_batches[i]));
  }
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

  ASSERT_OK_AND_ASSIGN(auto source_ds, ParquetDataSource::Open(EmployeesParquet().string()));
  auto shared_ds = std::make_shared<ParquetDataSource>(std::move(source_ds));

  TableScanOperator scan{0, shared_ds, std::nullopt, manager};
  CollectorOperator collector{1};
  auto graph = std::make_shared<Graph>(manager);
  // Full barrier forces a pipeline split so batches cross a BatchHolder
  // (Pipeline barrier alone still fuses).
  *graph += link(scan, collector, EdgeConfig::Simple().with_barrier(BarrierType::Full));

  ASSERT_OK(graph->Execute());
  EXPECT_GT(collector.total_rows(), 0U);
  EXPECT_GT(collector.disk_tier_count(), 0U)
      << "tiny CPU budget must spill at least one batch through the holder";
}

TEST(ConcatenatingBatchHolderTest, MultipleBlocksConcatenate) {
  ConcatenatingBatchHolder holder(1);
  ASSERT_OK(
      holder.Add(std::make_shared<HostMemoryBlock>("a", quest::testing::MakeTestRecordBatch(3))));
  ASSERT_OK(
      holder.Add(std::make_shared<HostMemoryBlock>("b", quest::testing::MakeTestRecordBatch(2))));
  holder.Finish();

  ASSERT_OK_AND_ASSIGN(auto merged, holder.Pull());
  EXPECT_EQ(merged->num_rows(), 5U);
}
