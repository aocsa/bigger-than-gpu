// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_execution_parity.cpp
/// @brief Stage 5 of docs/dev/pipeline-execution-plan.md: proves the
///        pipelined engine (Graph::ExecutePipelined) is a faithful
///        replacement for the legacy thread-per-operator engine
///        (Graph::Execute).
///
/// Three concerns, one file:
/// 1. Parity — the same plan, run through both engines with identical
///    dual-mode operators, produces identical batches in identical order.
/// 2. Spill — a cross-pipeline holder under a saturated CPU budget spills
///    to disk and the consumer still receives every row (the project's
///    namesake feature, exercised through the pipelined path).
/// 3. Benchmark — 10M rows through scan >> filter >> project >> collect,
///    fused pipeline vs thread-per-operator; numbers are printed and
///    recorded in tasks/todo.md's review log (this is a measurement, not
///    a hard assertion — CI variance makes perf asserts flaky).

#include <gtest/gtest.h>

#include <chrono>
#include <format>
#include <memory>
#include <print>
#include <quest/quest.hpp>
#include <string>
#include <utility>
#include <vector>

#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/pipeline.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

namespace {

/// @brief (block id, row count) — the unit of parity comparison.
using BatchFingerprint = std::pair<std::string, std::size_t>;

// =============================================================================
// Dual-mode test operators: implement BOTH engines' contracts so the same
// class runs under Execute() (run() + holders) and ExecutePipelined()
// (source/execute/sink roles). Parity is only meaningful if the two runs
// share the operator implementations.
// =============================================================================

/// @brief Source usable by both engines: run() pushes every batch to the
/// output holder (legacy); GetNextBatch() yields the same batches (new).
class DualSource : public Operator {
 public:
  DualSource(std::uint32_t operator_id, std::size_t batch_count, std::size_t rows_per_batch,
             std::shared_ptr<memory::CPUMemoryResource> cpu_resource = nullptr,
             std::shared_ptr<memory::DiskMemoryResource> disk_resource = nullptr)
      : Operator(operator_id, "dual_source", OperatorType::TableScan) {
    batches_.reserve(batch_count);
    for (std::size_t i = 0; i < batch_count; ++i) {
      auto block = std::make_shared<HostMemoryBlock>(
          std::format("batch_{}", i), quest::testing::MakeTestRecordBatch(rows_per_batch),
          cpu_resource);
      if (disk_resource) {
        block->set_disk_resource(disk_resource);
      }
      batches_.push_back(std::move(block));
    }
  }

  [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }

  // Legacy engine: push everything, then run() returns and the scheduler
  // finishes the output port.
  [[nodiscard]] ExecutionStatus run() override {
    for (auto& block : batches_) {
      if (!AddToOutput(block).ok()) {
        return ExecutionStatus::Stop;
      }
      WaitIfOutputSaturated();
    }
    return ExecutionStatus::Proceed;
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

/// @brief Streaming passthrough usable by both engines: SimpleOperator's
/// run() loop drives ProcessBatch on the legacy path; the pipeline driver
/// calls the same ProcessBatch on the new path.
class DualStream : public SimpleOperator {
 public:
  explicit DualStream(std::uint32_t operator_id)
      : SimpleOperator(operator_id, "dual_stream", OperatorType::Project) {}

  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr input) override {
    ++processed_count_;
    return input;
  }

  [[nodiscard]] std::size_t processed_count() const noexcept { return processed_count_; }

 private:
  std::size_t processed_count_{0};
};

/// @brief Blocking accumulate-then-reemit operator usable by both engines
/// (a stand-in for aggregate/sort). Legacy: run() pulls its entire input,
/// then re-emits downstream. Pipelined: Sink() accumulates, FinalizeSink()
/// flips to source mode, GetNextBatch() re-emits.
class DualBlockingOp : public Operator {
 public:
  explicit DualBlockingOp(std::uint32_t operator_id)
      : Operator(operator_id, "dual_blocking", OperatorType::ComputeAggregate) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  [[nodiscard]] ExecutionStatus run() override {
    auto input_holder = InputCache();
    if (!input_holder) {
      return ExecutionStatus::Stop;
    }
    while (true) {
      auto pulled = input_holder->Pull();
      if (!pulled.has_value()) {
        break;  // Finished-and-empty EOS (legacy protocol).
      }
      accumulated_.push_back(std::move(pulled.value()));
    }
    for (auto& block : accumulated_) {
      if (!AddToOutput(block).ok()) {
        return ExecutionStatus::Stop;
      }
    }
    return ExecutionStatus::Proceed;
  }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    accumulated_.push_back(std::move(batch));
    return Status::OK();
  }

  [[nodiscard]] Status FinalizeSink() override {
    finalized_ = true;
    return Status::OK();
  }

  [[nodiscard]] bool IsSource() const noexcept override { return finalized_; }

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (emit_index_ >= accumulated_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    return std::optional<DataBlockPtr>{accumulated_[emit_index_++]};
  }

 private:
  std::vector<DataBlockPtr> accumulated_;
  std::size_t emit_index_{0};
  bool finalized_{false};
};

/// @brief Terminal collector usable by both engines: run() pulls all
/// (legacy); IsSink()/Sink() collects (pipelined). Records fingerprints in
/// arrival order for parity comparison.
class DualCollector : public Operator {
 public:
  explicit DualCollector(std::uint32_t operator_id)
      : Operator(operator_id, "dual_collector", OperatorType::Output) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }
  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] ExecutionStatus run() override {
    auto input_holder = InputCache();
    if (!input_holder) {
      return ExecutionStatus::Stop;
    }
    while (true) {
      auto pulled = input_holder->Pull();
      if (!pulled.has_value()) {
        break;
      }
      Record(pulled.value());
    }
    return ExecutionStatus::Proceed;
  }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    Record(batch);
    return Status::OK();
  }

  [[nodiscard]] const std::vector<BatchFingerprint>& fingerprints() const noexcept {
    return fingerprints_;
  }

  [[nodiscard]] std::size_t total_rows() const noexcept { return total_rows_; }

  [[nodiscard]] std::size_t disk_tier_count() const noexcept { return disk_tier_count_; }

 private:
  void Record(const DataBlockPtr& block) {
    if (!block) {
      return;
    }
    fingerprints_.emplace_back(std::string(block->id()), block->num_rows());
    total_rows_ += block->num_rows();
    if (block->tier() == memory::MemoryTier::Disk) {
      ++disk_tier_count_;
    }
  }

  std::vector<BatchFingerprint> fingerprints_;
  std::size_t total_rows_{0};
  std::size_t disk_tier_count_{0};
};

// =============================================================================
// 1. Parity — identical results from both engines
// =============================================================================

TEST(ExecutionParityTest, StreamingPlanProducesIdenticalBatchesOnBothEngines) {
  constexpr std::size_t kBatches = 8;
  constexpr std::size_t kRows = 16;

  // Two identical plans with independent operator instances (operators
  // hold execution state and cannot be reused across runs).
  DualSource legacy_source{0, kBatches, kRows};
  DualStream legacy_stream{1};
  DualCollector legacy_collector{2};
  {
    GraphBuilder gb;
    gb.Chain(legacy_source, legacy_stream, legacy_collector);
    ASSERT_OK_AND_ASSIGN(auto legacy_graph, std::move(gb).Build());
    ASSERT_OK(legacy_graph->ExecuteLegacy());
  }

  DualSource pipelined_source{0, kBatches, kRows};
  DualStream pipelined_stream{1};
  DualCollector pipelined_collector{2};
  {
    GraphBuilder gb;
    gb.Chain(pipelined_source, pipelined_stream, pipelined_collector);
    ASSERT_OK_AND_ASSIGN(auto pipelined_graph, std::move(gb).Build());
    ASSERT_OK(pipelined_graph->ExecutePipelined());
  }

  EXPECT_EQ(legacy_collector.fingerprints().size(), kBatches);
  EXPECT_EQ(legacy_collector.fingerprints(), pipelined_collector.fingerprints())
      << "the two engines must deliver the same batches in the same order";
  EXPECT_EQ(legacy_stream.processed_count(), pipelined_stream.processed_count());
}

TEST(ExecutionParityTest, BlockingPlanProducesIdenticalBatchesOnBothEngines) {
  constexpr std::size_t kBatches = 6;
  constexpr std::size_t kRows = 12;

  DualSource legacy_source{0, kBatches, kRows};
  DualBlockingOp legacy_agg{1};
  DualCollector legacy_collector{2};
  {
    GraphBuilder gb;
    gb.Chain(legacy_source, legacy_agg, legacy_collector);
    ASSERT_OK_AND_ASSIGN(auto legacy_graph, std::move(gb).Build());
    ASSERT_OK(legacy_graph->ExecuteLegacy());
  }

  DualSource pipelined_source{0, kBatches, kRows};
  DualBlockingOp pipelined_agg{1};
  DualCollector pipelined_collector{2};
  {
    GraphBuilder gb;
    gb.Chain(pipelined_source, pipelined_agg, pipelined_collector);
    ASSERT_OK_AND_ASSIGN(auto pipelined_graph, std::move(gb).Build());
    ASSERT_OK(pipelined_graph->ExecutePipelined());
  }

  EXPECT_EQ(legacy_collector.fingerprints().size(), kBatches);
  EXPECT_EQ(legacy_collector.fingerprints(), pipelined_collector.fingerprints());
}

// =============================================================================
// 2. Spill — a saturated CPU budget spills through the cross-pipeline
// holder, and every row still arrives
// =============================================================================

TEST(ExecutionParityTest, PipelinedPathSpillsToDiskUnderTinyBudgetAndLosesNothing) {
  constexpr std::size_t kBatches = 8;
  constexpr std::size_t kRows = 10;

  // A CPU budget small enough that the pre-created source batches saturate
  // it immediately (each ~kRows * 8 bytes by the estimate), so every
  // holder Add() sees should_spill() == true.
  auto cpu = std::make_shared<memory::CPUMemoryResource>(
      memory::CPUMemoryConfig{.max_bytes = 256, .saturation_threshold = 0.5});
  auto disk = std::make_shared<memory::DiskMemoryResource>();
  auto manager = std::make_shared<memory::MemoryResourceManager>(cpu, disk);

  // Blocks carry both resources: CPU (allocation tracking -> saturation)
  // and disk (ToDisk() needs a target to generate spill files).
  DualSource producer_op{0, kBatches, kRows, cpu, disk};
  DualCollector consumer_op{1};
  {
    GraphBuilder gb{manager};
    gb.Chain(producer_op, consumer_op);
    ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
    graph->Materialize();
  }

  ASSERT_TRUE(cpu->is_saturated())
      << "test setup: pre-created batches must already saturate the budget";

  // Hand-assembled Pipeline-barrier split (a plain edge would fuse and
  // bypass the holder entirely): producer publishes through the holder,
  // consumer claims from it. Producer precedes consumer (ordering
  // invariant on PipelineExecutor::Execute).
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

  EXPECT_EQ(consumer_op.fingerprints().size(), kBatches) << "no batch may be lost to spilling";
  EXPECT_EQ(consumer_op.total_rows(), kBatches * kRows);
  EXPECT_GT(consumer_op.disk_tier_count(), 0U)
      << "with a saturated CPU budget, at least some batches must arrive "
         "as spilled DiskBlocks";
}

// =============================================================================
// 3. Benchmark — 10M rows, fused pipeline vs thread-per-operator.
// Numbers are printed for the record; the only hard assertions are
// correctness (both engines deliver all rows).
// =============================================================================

TEST(ExecutionParityTest, Benchmark10MRowsFusedVsThreadPerOperator) {
  // 10M rows in MANY SMALL batches: with passthrough operators, what the
  // two engines actually differ in is per-batch overhead (queue handoffs +
  // thread wakeups vs one fused call chain), so the batch count — not the
  // row count — is what makes the comparison visible.
  constexpr std::size_t kBatches = 10'000;
  constexpr std::size_t kRowsPerBatch = 1'000;  // 10M rows total
  constexpr std::size_t kTotalRows = kBatches * kRowsPerBatch;

  auto run_legacy = [&]() -> std::pair<std::chrono::milliseconds, std::size_t> {
    DualSource source{0, kBatches, kRowsPerBatch};
    DualStream filter{1};
    DualStream project{2};
    DualCollector collector{3};

    GraphBuilder gb;
    gb.Chain(source, filter, project, collector);
    auto graph_result = std::move(gb).Build();
    EXPECT_TRUE(graph_result.has_value());
    if (!graph_result.has_value()) {
      return {{}, 0};
    }
    auto graph = *graph_result;

    auto start = std::chrono::steady_clock::now();
    auto status = graph->ExecuteLegacy();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_TRUE(status.has_value());
    return {elapsed, collector.total_rows()};
  };

  auto run_pipelined = [&]() -> std::pair<std::chrono::milliseconds, std::size_t> {
    DualSource source{0, kBatches, kRowsPerBatch};
    DualStream filter{1};
    DualStream project{2};
    DualCollector collector{3};

    GraphBuilder gb;
    gb.Chain(source, filter, project, collector);
    auto graph_result = std::move(gb).Build();
    EXPECT_TRUE(graph_result.has_value());
    if (!graph_result.has_value()) {
      return {{}, 0};
    }
    auto graph = *graph_result;

    auto start = std::chrono::steady_clock::now();
    auto status = graph->ExecutePipelined();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);
    EXPECT_TRUE(status.has_value());
    return {elapsed, collector.total_rows()};
  };

  auto [legacy_ms, legacy_rows] = run_legacy();
  auto [pipelined_ms, pipelined_rows] = run_pipelined();

  EXPECT_EQ(legacy_rows, kTotalRows);
  EXPECT_EQ(pipelined_rows, kTotalRows);

  std::println("[benchmark] 10M rows, scan >> filter >> project >> collect");
  std::println("[benchmark]   thread-per-operator (Execute):        {} ms", legacy_ms.count());
  std::println("[benchmark]   fused pipeline (ExecutePipelined):    {} ms", pipelined_ms.count());
}

}  // namespace
