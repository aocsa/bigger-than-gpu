// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_pipeline_spill.cpp
/// @brief Spill regression for a holder-fed pipeline pair.

#include <gtest/gtest.h>

#include <format>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/pipeline.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::taskflow;

namespace {

struct SpillWorkload {
  std::size_t batch_count;
  std::size_t rows_per_batch;
};

class SpillSource : public Operator {
 public:
  SpillSource(std::uint32_t operator_id, SpillWorkload workload,
              const std::shared_ptr<memory::CPUMemoryResource>& cpu_resource,
              const std::shared_ptr<memory::DiskMemoryResource>& disk_resource)
      : Operator(operator_id, "spill_source", OperatorType::TableScan) {
    batches_.reserve(workload.batch_count);
    for (std::size_t i = 0; i < workload.batch_count; ++i) {
      auto block = std::make_shared<HostMemoryBlock>(
          std::format("batch_{}", i), quest::testing::MakeTestRecordBatch(workload.rows_per_batch),
          cpu_resource);
      block->set_disk_resource(disk_resource);
      batches_.push_back(std::move(block));
    }
  }

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (next_index_ >= batches_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    return std::optional<DataBlockPtr>{batches_[next_index_++]};
  }

 private:
  std::vector<DataBlockPtr> batches_;
  std::size_t next_index_{0};
};

class SpillCollector : public Operator {
 public:
  explicit SpillCollector(std::uint32_t operator_id)
      : Operator(operator_id, "spill_collector", OperatorType::Output) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    if (!batch) {
      return Status::OK();
    }
    ++batch_count_;
    total_rows_ += batch->num_rows();
    if (batch->tier() == memory::MemoryTier::Disk) {
      ++disk_tier_count_;
    }
    return Status::OK();
  }

  [[nodiscard]] std::size_t batch_count() const noexcept { return batch_count_; }
  [[nodiscard]] std::size_t total_rows() const noexcept { return total_rows_; }
  [[nodiscard]] std::size_t disk_tier_count() const noexcept { return disk_tier_count_; }

 private:
  std::size_t batch_count_{0};
  std::size_t total_rows_{0};
  std::size_t disk_tier_count_{0};
};

TEST(PipelineSpillTest, SaturatedBudgetSpillsWithoutLosingRows) {
  constexpr std::size_t kBatches = 8;
  constexpr std::size_t kRows = 10;
  constexpr std::size_t kCpuBudgetBytes = 256;
  constexpr double kSaturationThreshold = 0.5;

  auto cpu = std::make_shared<memory::CPUMemoryResource>(memory::CPUMemoryConfig{
      .max_bytes = kCpuBudgetBytes, .saturation_threshold = kSaturationThreshold});
  auto disk = std::make_shared<memory::DiskMemoryResource>();
  auto manager = std::make_shared<memory::MemoryResourceManager>(cpu, disk);

  SpillSource producer_op{0, SpillWorkload{.batch_count = kBatches, .rows_per_batch = kRows}, cpu,
                          disk};
  SpillCollector consumer_op{1};
  {
    GraphBuilder builder{manager};
    builder.Chain(producer_op, consumer_op);
    ASSERT_OK_AND_ASSIGN(auto graph, std::move(builder).Build());
    graph->Materialize();
  }

  ASSERT_TRUE(cpu->is_saturated());

  auto producer_pipeline = std::make_unique<Pipeline>(0);
  producer_pipeline->PrependOperator(&producer_op);
  auto consumer_pipeline = std::make_unique<Pipeline>(1);
  consumer_pipeline->PrependOperator(&consumer_op);
  consumer_pipeline->AddProducer(producer_pipeline.get(), BarrierType::Pipeline);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(producer_pipeline));
  pipelines.push_back(std::move(consumer_pipeline));

  PipelineExecutor executor{2};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  EXPECT_EQ(consumer_op.batch_count(), kBatches);
  EXPECT_EQ(consumer_op.total_rows(), kBatches * kRows);
  EXPECT_GT(consumer_op.disk_tier_count(), 0U);
}

}  // namespace
