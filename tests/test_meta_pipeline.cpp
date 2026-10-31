// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_meta_pipeline.cpp
/// @brief Shape tests for Pipeline + MetaPipeline::Build, Stage 3 of
///        docs/dev/pipeline-execution-plan.md.
///
/// Tests 1-5 and 7 are build-only: they assert the pipeline structure
/// MetaPipeline::Build produces from a Graph, never executing anything.
/// Test 6 exercises Pipeline::RunTask's push loop directly (no graph, no
/// MetaPipeline involved) against hand-assembled operators.

#include <gtest/gtest.h>

#include <memory>
#include <quest/quest.hpp>
#include <string>
#include <vector>

#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/pipeline.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

namespace {

[[nodiscard]] DataBlockPtr MakeTestBlock(const std::string& block_id, std::size_t rows) {
  auto batch = quest::testing::MakeTestRecordBatch(rows);
  return std::make_shared<HostMemoryBlock>(block_id, std::move(batch));
}

// =============================================================================
// Test Doubles
// =============================================================================

/// @brief Identity-only mock for shape tests: MetaPipeline::Build reads
/// structure (id, type, edges), never data, so ProcessBatch/Sink are never
/// invoked here.
class MockOperator : public Operator {
 public:
  MockOperator(std::uint32_t id, OperatorType type) : Operator(id, "mock", type) {}
};

/// @brief Streaming double for RunTask: passthrough that counts calls.
class CountingOperator : public Operator {
 public:
  explicit CountingOperator(std::uint32_t id) : Operator(id, "counting", OperatorType::Filter) {}
  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr input) override {
    ++processed_count_;
    return input;
  }
  [[nodiscard]] std::size_t processed_count() const noexcept { return processed_count_; }

 private:
  std::size_t processed_count_{0};
};

/// @brief Streaming double that always errors, for the error-propagation
/// test.
class FailingOperator : public Operator {
 public:
  explicit FailingOperator(std::uint32_t id) : Operator(id, "failing", OperatorType::Filter) {}
  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr) override {
    return Err(ErrorCode::Invalid, "FailingOperator always fails");
  }
};

/// @brief Blocking-sink double: accumulates via Sink(). Stand-in for a
/// real aggregate/sort (same role as test_operator_roles.cpp's
/// AccumulatingSinkOperator, kept file-local per the Stage 2 test file).
class AccumulatingSink : public Operator {
 public:
  explicit AccumulatingSink(std::uint32_t id)
      : Operator(id, "accumulating_sink", OperatorType::ComputeAggregate) {}
  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    accumulated_.push_back(std::move(batch));
    return {};
  }
  [[nodiscard]] std::size_t accumulated_count() const noexcept { return accumulated_.size(); }

 private:
  std::vector<DataBlockPtr> accumulated_;
};

/// @brief Non-blocking streaming double used as a pipeline's sink to
/// exercise PublishOutput's ProcessBatch-then-AddToOutput branch.
class PassthroughOperator : public SimpleOperator {
 public:
  explicit PassthroughOperator(std::uint32_t id)
      : SimpleOperator(id, "passthrough", OperatorType::Project) {}

 protected:
  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr input) override { return input; }
};

// =============================================================================
// Shape test 1 — linear plan fuses into one pipeline
// =============================================================================

TEST(MetaPipelineTest, LinearPlanFusesIntoOnePipeline) {
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator filter{1, OperatorType::Filter};
  MockOperator project{2, OperatorType::Project};

  GraphBuilder gb;
  gb.Chain(scan, filter, project);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  const auto& pipelines = result.value();

  ASSERT_EQ(pipelines.size(), 1U);
  const auto& ops = pipelines[0]->operators();
  ASSERT_EQ(ops.size(), 3U);
  EXPECT_EQ(ops[0], &scan);
  EXPECT_EQ(ops[1], &filter);
  EXPECT_EQ(ops[2], &project);
  EXPECT_EQ(pipelines[0]->source(), &scan);
  EXPECT_EQ(pipelines[0]->sink(), &project);
  EXPECT_TRUE(pipelines[0]->FullBarrierProducers().empty());
}

// =============================================================================
// Shape test 2 — a blocking operator splits into two pipelines, appears
// in both, with a FULL dependency
// =============================================================================

TEST(MetaPipelineTest, BlockingOperatorSplitsIntoTwoPipelinesWithFullDep) {
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator filter{1, OperatorType::Filter};
  MockOperator agg{2, OperatorType::ComputeAggregate};  // blocking
  MockOperator project{3, OperatorType::Project};

  GraphBuilder gb;
  gb.Chain(scan, filter, agg, project);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  const auto& pipelines = result.value();

  ASSERT_EQ(pipelines.size(), 2U);

  // Children are appended before the parent that depends on them.
  const auto& child = pipelines[0];
  const auto& parent = pipelines[1];

  EXPECT_EQ(child->operators(), (std::vector<Operator*>{&scan, &filter, &agg}));
  EXPECT_EQ(child->sink(), &agg);

  EXPECT_EQ(parent->operators(), (std::vector<Operator*>{&agg, &project}));
  EXPECT_EQ(parent->source(), &agg);  // agg: sink of one, source of the next

  ASSERT_EQ(parent->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(parent->FullBarrierProducers()[0], child.get());
  EXPECT_TRUE(parent->PipelineBarrierProducers().empty());
}

// =============================================================================
// Shape test 3 — a multi-input operator gets one child pipeline per input
// port, in deterministic (target-port-name) order, children before parent
// =============================================================================

TEST(MetaPipelineTest, MultiInputOperatorGetsOneChildPerPortInOrder) {
  MockOperator source_a{0, OperatorType::TableScan};
  MockOperator source_b{1, OperatorType::TableScan};
  MockOperator join{2, OperatorType::PartwiseJoin};

  GraphBuilder gb;
  gb.Edge(source_a, join["input_a"]);
  gb.Edge(source_b, join["input_b"]);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  const auto& pipelines = result.value();

  // Two single-operator source pipelines plus the join's own pipeline.
  ASSERT_EQ(pipelines.size(), 3U);

  const auto& child_a = pipelines[0];
  const auto& child_b = pipelines[1];
  const auto& parent = pipelines[2];

  EXPECT_EQ(child_a->operators(), (std::vector<Operator*>{&source_a}));
  EXPECT_EQ(child_b->operators(), (std::vector<Operator*>{&source_b}));
  EXPECT_EQ(parent->operators(), (std::vector<Operator*>{&join}));

  // input_a's child is registered before input_b's (port-name order).
  ASSERT_EQ(parent->PipelineBarrierProducers().size(), 2U);
  EXPECT_EQ(parent->PipelineBarrierProducers()[0], child_a.get());
  EXPECT_EQ(parent->PipelineBarrierProducers()[1], child_b.get());
  EXPECT_TRUE(parent->FullBarrierProducers().empty());
}

// =============================================================================
// Shape test 4 — an explicit Full barrier splits a pipeline even without
// a blocking operator; the upstream operator is NOT shared with the parent
// =============================================================================

TEST(MetaPipelineTest, ExplicitFullBarrierSplitsWithoutBlockingOperator) {
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator filter{1, OperatorType::Filter};  // not blocking
  MockOperator out{2, OperatorType::Output};

  GraphBuilder gb;
  gb.Chain(scan, filter);
  gb.Edge(filter, out).Full();
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  const auto& pipelines = result.value();

  ASSERT_EQ(pipelines.size(), 2U);
  const auto& child = pipelines[0];
  const auto& parent = pipelines[1];

  EXPECT_EQ(child->operators(), (std::vector<Operator*>{&scan, &filter}));
  EXPECT_EQ(parent->operators(), (std::vector<Operator*>{&out}));  // filter NOT shared

  ASSERT_EQ(parent->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(parent->FullBarrierProducers()[0], child.get());
}

// =============================================================================
// Shape test 5 — a partitioned edge splits a pipeline under a PIPELINE
// (not FULL) barrier
// =============================================================================

TEST(MetaPipelineTest, PartitionedEdgeSplitsUnderPipelineBarrier) {
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator worker{1, OperatorType::Project};

  GraphBuilder gb;
  gb.Edge(scan, worker).Partitioned(2);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  const auto& pipelines = result.value();

  ASSERT_EQ(pipelines.size(), 2U);
  const auto& child = pipelines[0];
  const auto& parent = pipelines[1];

  EXPECT_EQ(child->operators(), (std::vector<Operator*>{&scan}));
  EXPECT_EQ(parent->operators(), (std::vector<Operator*>{&worker}));

  EXPECT_TRUE(parent->FullBarrierProducers().empty());
  ASSERT_EQ(parent->PipelineBarrierProducers().size(), 1U);
  EXPECT_EQ(parent->PipelineBarrierProducers()[0], child.get());
}

// =============================================================================
// Shape test 7 — a shared blocking producer feeding two consumers builds
// its child pipeline exactly once (memoized)
// =============================================================================

TEST(MetaPipelineTest, SharedBlockingProducerIsBuiltOnce) {
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator agg{1, OperatorType::ComputeAggregate};  // blocking, shared
  MockOperator project_a{2, OperatorType::Project};
  MockOperator project_b{3, OperatorType::Project};

  GraphBuilder gb;
  gb.Edge(scan, agg);
  gb.Edge(agg, project_a);
  gb.Edge(agg, project_b);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  const auto& pipelines = result.value();

  // [scan, agg] built once, plus [agg, project_a] and [agg, project_b].
  ASSERT_EQ(pipelines.size(), 3U);

  Pipeline* shared_child = nullptr;
  std::vector<Pipeline*> parents;
  for (const auto& pipeline : pipelines) {
    if (pipeline->sink() == &agg) {
      shared_child = pipeline.get();
    } else {
      parents.push_back(pipeline.get());
    }
  }

  ASSERT_NE(shared_child, nullptr);
  ASSERT_EQ(parents.size(), 2U);
  for (auto* parent : parents) {
    ASSERT_EQ(parent->FullBarrierProducers().size(), 1U);
    EXPECT_EQ(parent->FullBarrierProducers()[0], shared_child);
  }
}

// =============================================================================
// Robustness — terminal detection, unsupported shapes, cycles
// =============================================================================

TEST(MetaPipelineTest, BuildFindsTerminalOperator) {
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator filter{1, OperatorType::Filter};

  GraphBuilder gb;
  gb.Chain(scan, filter);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(result.has_value());
  ASSERT_EQ(result.value().size(), 1U);
  EXPECT_EQ(result.value()[0]->sink(), &filter);
}

TEST(MetaPipelineTest, BlockingProducerIntoMultiInputConsumerIsRejected) {
  // agg accumulates via Sink(); nothing would ever re-emit its state into
  // the join's input port (no relay pipeline yet), so Build must fail
  // loudly instead of producing an unexecutable shape.
  MockOperator scan{0, OperatorType::TableScan};
  MockOperator agg{1, OperatorType::ComputeAggregate};  // blocking
  MockOperator source_b{2, OperatorType::TableScan};
  MockOperator join{3, OperatorType::PartwiseJoin};

  GraphBuilder gb;
  gb.Edge(scan, agg);
  gb.Edge(agg, join["input_a"]);
  gb.Edge(source_b, join["input_b"]);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto result = MetaPipeline::Build(*graph);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Invalid);
}

TEST(MetaPipelineTest, StreamingCycleReturnsErrorInsteadOfHanging) {
  // a -> b -> a plus b -> out: the fuse walk from `out` would loop
  // forever without the per-walk visited set.
  auto graph = std::make_shared<Graph>();
  MockOperator op_a{0, OperatorType::Filter};
  MockOperator op_b{1, OperatorType::Filter};
  MockOperator out{2, OperatorType::Output};

  graph->AddEdge(&op_a, &op_b, "0", "1", EdgeConfig{});
  graph->AddEdge(&op_b, &op_a, "1", "0", EdgeConfig{});
  graph->AddEdge(&op_b, &out, "1", "2", EdgeConfig{});

  auto result = MetaPipeline::Build(*graph);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Invalid);
}

TEST(MetaPipelineTest, BlockingCycleReturnsErrorInsteadOfOverflowing) {
  // agg1 -> agg2 -> agg1 plus agg2 -> out: BuildFrom recursion would
  // re-enter agg2 without the in-progress guard.
  auto graph = std::make_shared<Graph>();
  MockOperator agg1{0, OperatorType::ComputeAggregate};
  MockOperator agg2{1, OperatorType::ComputeAggregate};
  MockOperator out{2, OperatorType::Output};

  graph->AddEdge(&agg1, &agg2, "0", "1", EdgeConfig{});
  graph->AddEdge(&agg2, &agg1, "1", "0", EdgeConfig{});
  graph->AddEdge(&agg2, &out, "1", "2", EdgeConfig{});

  auto result = MetaPipeline::Build(*graph);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Invalid);
}

// =============================================================================
// Shape test 6 — Pipeline::RunTask push loop (no Graph/MetaPipeline)
// =============================================================================

TEST(PipelineRunTaskTest, PushesBatchThroughStreamingOpsIntoSink) {
  MockOperator source_placeholder{0, OperatorType::TableScan};
  CountingOperator stream_a{1};
  CountingOperator stream_b{2};
  AccumulatingSink sink{3};

  Pipeline pipeline{0};
  pipeline.PrependOperator(&sink);
  pipeline.PrependOperator(&stream_b);
  pipeline.PrependOperator(&stream_a);
  pipeline.PrependOperator(&source_placeholder);
  ASSERT_EQ(pipeline.operators(),
            (std::vector<Operator*>{&source_placeholder, &stream_a, &stream_b, &sink}));

  auto batch = MakeTestBlock("b0", 4);
  auto status = pipeline.RunTask(batch);  // default start_index = 1

  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(stream_a.processed_count(), 1U);
  EXPECT_EQ(stream_b.processed_count(), 1U);
  EXPECT_EQ(sink.accumulated_count(), 1U);
}

TEST(PipelineRunTaskTest, ErrorMidLoopPropagatesAndSkipsSink) {
  MockOperator source_placeholder{0, OperatorType::TableScan};
  FailingOperator failing{1};
  AccumulatingSink sink{2};

  Pipeline pipeline{0};
  pipeline.PrependOperator(&sink);
  pipeline.PrependOperator(&failing);
  pipeline.PrependOperator(&source_placeholder);

  auto status = pipeline.RunTask(MakeTestBlock("b0", 4));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(sink.accumulated_count(), 0U);  // never reached
}

TEST(PipelineRunTaskTest, SingleOperatorPipelinePublishesDirectlyToSink) {
  AccumulatingSink sink{0};
  Pipeline pipeline{0};
  pipeline.PrependOperator(&sink);
  ASSERT_EQ(pipeline.operators(), (std::vector<Operator*>{&sink}));

  auto status = pipeline.RunTask(MakeTestBlock("b0", 4));
  ASSERT_TRUE(status.has_value());
  EXPECT_EQ(sink.accumulated_count(), 1U);
}

TEST(PipelineRunTaskTest, NonBlockingSinkRunsProcessBatchThenAddToOutput) {
  // PassthroughOperator is the pipeline's sink but is NOT blocking:
  // PublishOutput must call ProcessBatch then AddToOutput, which requires
  // a real registered output port -- build a tiny graph so AddEdge wires
  // one, then hand-assemble the Pipeline around the same operators.
  MockOperator scan{0, OperatorType::TableScan};
  PassthroughOperator sink{1};
  MockOperator downstream{2, OperatorType::Output};

  GraphBuilder gb;
  gb.Chain(scan, sink, downstream);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();  // holders deferred from AddEdge until Prepare/Materialize

  Pipeline pipeline{0};
  pipeline.PrependOperator(&sink);
  pipeline.PrependOperator(&scan);

  auto status = pipeline.RunTask(MakeTestBlock("b0", 4));
  ASSERT_TRUE(status.has_value());

  // The batch crossed into the downstream holder via AddToOutput.
  auto downstream_holder = downstream.InputCache();
  ASSERT_NE(downstream_holder, nullptr);
  EXPECT_TRUE(downstream_holder->HasNext());
}

}  // namespace
