// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_pipeline_executor.cpp
/// @brief Tests for PipelineExecutor, Stage 4 of
///        docs/dev/pipeline-execution-plan.md.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <quest/quest.hpp>
#include <string>
#include <thread>
#include <vector>

#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/pipeline.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
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
// Shared test helper: a thread-safe, order-preserving event log.
//
// Concurrency assertions in this file are made by comparing the relative
// position of named events in one shared log, not by sleeping and hoping
// -- e.g. "sink_finalize" must appear after every "source_yield_*" and
// before every "project_process_*", which holds by construction of the
// executor regardless of scheduling jitter.
// =============================================================================

class EventLog {
 public:
  void Record(std::string event) {
    std::lock_guard<std::mutex> lock(mutex_);
    events_.push_back(std::move(event));
  }

  // -1 if the event was never recorded.
  [[nodiscard]] int IndexOf(const std::string& event) const {
    std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < events_.size(); ++i) {
      if (events_[i] == event) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  [[nodiscard]] std::vector<std::string> Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return events_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<std::string> events_;
};

// =============================================================================
// Test doubles
// =============================================================================

/// @brief Source that logs every yield and the final EOS.
class LoggingSourceOperator : public Operator {
 public:
  LoggingSourceOperator(std::uint32_t id, std::vector<DataBlockPtr> batches, EventLog& log,
                        std::string tag)
      : Operator(id, "logging_source", OperatorType::TableScan),
        batches_(std::move(batches)),
        log_(log),
        tag_(std::move(tag)) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (index_ >= batches_.size()) {
      log_.Record(tag_ + "_eos");
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    log_.Record(tag_ + "_yield_" + std::to_string(index_));
    return std::optional<DataBlockPtr>{batches_[index_++]};
  }

 private:
  std::vector<DataBlockPtr> batches_;
  EventLog& log_;
  std::string tag_;
  std::size_t index_{0};
};

/// @brief Blocking sink/source mock (like Stage 2/3's AccumulatingSink)
/// that also logs Sink()/FinalizeSink()/re-emit calls.
class LoggingAccumulatingSink : public Operator {
 public:
  LoggingAccumulatingSink(std::uint32_t id, EventLog& log, std::string tag)
      : Operator(id, "logging_accumulating_sink", OperatorType::ComputeAggregate),
        log_(log),
        tag_(std::move(tag)) {}

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    log_.Record(tag_ + "_sink_" + std::to_string(accumulated_.size()));
    accumulated_.push_back(std::move(batch));
    return {};
  }

  [[nodiscard]] Status FinalizeSink() override {
    log_.Record(tag_ + "_finalize");
    finalized_ = true;
    return {};
  }

  [[nodiscard]] bool IsSource() const noexcept override { return finalized_; }

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (emit_index_ >= accumulated_.size()) {
      log_.Record(tag_ + "_reemit_eos");
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    log_.Record(tag_ + "_reemit_" + std::to_string(emit_index_));
    return std::optional<DataBlockPtr>{accumulated_[emit_index_++]};
  }

  [[nodiscard]] std::size_t accumulated_count() const noexcept { return accumulated_.size(); }

 private:
  EventLog& log_;
  std::string tag_;
  std::vector<DataBlockPtr> accumulated_;
  std::size_t emit_index_{0};
  bool finalized_{false};
};

/// @brief Streaming operator that logs every ProcessBatch call.
///
/// Only valid as a pipeline's *intermediate* operator (RunTask's forward
/// loop calls ProcessBatch on it directly). Must never end up as a
/// pipeline's sink(): being non-blocking, PublishOutput would run
/// ProcessBatch then AddToOutput, and AddToOutput throws if this operator
/// has no real, wired output port -- use LoggingCollectingSink for a
/// pipeline's terminal position instead.
class LoggingStreamOperator : public Operator {
 public:
  LoggingStreamOperator(std::uint32_t id, EventLog& log, std::string tag)
      : Operator(id, "logging_stream", OperatorType::Project), log_(log), tag_(std::move(tag)) {}

  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr input) override {
    log_.Record(tag_ + "_process_" + std::to_string(processed_count_++));
    return input;
  }

  [[nodiscard]] std::size_t processed_count() const noexcept { return processed_count_; }

 private:
  EventLog& log_;
  std::string tag_;
  std::size_t processed_count_{0};
};

/// @brief Terminal collector that logs every Sink() call -- the
/// port-free counterpart to LoggingStreamOperator for a pipeline's sink()
/// position (IsSink()==true takes PublishOutput's accumulate branch, so
/// no wired output port is required).
class LoggingCollectingSink : public Operator {
 public:
  LoggingCollectingSink(std::uint32_t id, EventLog& log, std::string tag)
      : Operator(id, "logging_collecting_sink", OperatorType::Output),
        log_(log),
        tag_(std::move(tag)) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    log_.Record(tag_ + "_sink_" + std::to_string(received_.size()));
    received_.push_back(std::move(batch));
    return {};
  }

  [[nodiscard]] std::size_t received_count() const noexcept { return received_.size(); }

 private:
  EventLog& log_;
  std::string tag_;
  std::vector<DataBlockPtr> received_;
};

/// @brief Streaming double that always fails, for error-propagation tests.
class FailingOperator : public Operator {
 public:
  explicit FailingOperator(std::uint32_t id) : Operator(id, "failing", OperatorType::Filter) {}
  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr) override {
    return Err(ErrorCode::Invalid, "FailingOperator always fails");
  }
};

/// @brief Terminal collector: records every batch it receives, thread-safe
/// and query-able so tests can assert exact counts across pipelines that
/// ran concurrently.
class CollectingSink : public Operator {
 public:
  explicit CollectingSink(std::uint32_t id)
      : Operator(id, "collecting_sink", OperatorType::Output) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    received_.push_back(std::move(batch));
    return {};
  }

  [[nodiscard]] std::size_t received_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::vector<DataBlockPtr> received_;
};

/// @brief Source with N synthetic batches, for stress/bound tests.
class VectorSourceOperator : public Operator {
 public:
  VectorSourceOperator(std::uint32_t id, std::size_t batch_count)
      : Operator(id, "vector_source", OperatorType::TableScan), remaining_(batch_count) {}

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (remaining_ == 0) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    --remaining_;
    return std::optional<DataBlockPtr>{MakeTestBlock("b" + std::to_string(remaining_), 4)};
  }

 private:
  std::size_t remaining_;
};

/// @brief Source that hands out two fixed batches, blocking before the
/// second one on a shared future -- lets a test observe genuine overlap
/// with a concurrently-running consumer instead of inferring it from
/// timing.
class GatedTwoBatchSource : public Operator {
 public:
  GatedTwoBatchSource(std::uint32_t id, DataBlockPtr first, DataBlockPtr second,
                      std::shared_future<void> release_second)
      : Operator(id, "gated_source", OperatorType::TableScan),
        batches_{std::move(first), std::move(second)},
        release_second_(std::move(release_second)) {}

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (index_ >= batches_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    if (index_ == 1) {
      release_second_.wait();  // held here until the test releases us.
    }
    return std::optional<DataBlockPtr>{batches_[index_++]};
  }

 private:
  std::vector<DataBlockPtr> batches_;
  std::shared_future<void> release_second_;
  std::size_t index_{0};
};

/// @brief Sink with a wait-for-count helper, so a test can block until a
/// concurrently-running producer has delivered at least N batches without
/// polling on a timer.
class WaitableSink : public Operator {
 public:
  explicit WaitableSink(std::uint32_t id) : Operator(id, "waitable_sink", OperatorType::Filter) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    std::lock_guard<std::mutex> lock(mutex_);
    received_.push_back(std::move(batch));
    cv_.notify_all();
    return {};
  }

  [[nodiscard]] bool WaitForAtLeast(std::size_t count, std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [&] { return received_.size() >= count; });
  }

  [[nodiscard]] std::size_t received_count() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return received_.size();
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::vector<DataBlockPtr> received_;
};

// =============================================================================
// Test 1 -- FULL gating: consumer never observes the producer unfinished
// =============================================================================

TEST(PipelineExecutorTest, FullGatingConsumerStartsStrictlyAfterProducerFinishes) {
  EventLog log;

  LoggingSourceOperator scan{0, {MakeTestBlock("b0", 4), MakeTestBlock("b1", 4)}, log, "source"};
  LoggingAccumulatingSink agg{1, log, "sink"};
  LoggingCollectingSink project{2, log, "project"};

  GraphBuilder gb;
  gb.Edge(scan, agg);
  gb.Edge(agg, project);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto build_result = MetaPipeline::Build(*graph);
  ASSERT_TRUE(build_result.has_value());

  PipelineExecutor executor{2};
  auto status = executor.Execute(std::move(build_result.value()));
  ASSERT_TRUE(status.has_value()) << status.error().ToString();

  EXPECT_EQ(agg.accumulated_count(), 2U);
  EXPECT_EQ(project.received_count(), 2U);

  auto finalize_index = log.IndexOf("sink_finalize");
  ASSERT_GE(finalize_index, 0);

  // Every producer-side event happened before the consumer flipped to
  // source mode, and every consumer-side event happened after.
  for (const auto& event :
       {"source_yield_0", "source_yield_1", "source_eos", "sink_sink_0", "sink_sink_1"}) {
    auto index = log.IndexOf(event);
    ASSERT_GE(index, 0) << event;
    EXPECT_LT(index, finalize_index) << event << " should precede sink_finalize";
  }
  for (const auto& event : {"project_sink_0", "project_sink_1"}) {
    auto index = log.IndexOf(event);
    ASSERT_GE(index, 0) << event;
    EXPECT_GT(index, finalize_index) << event << " should follow sink_finalize";
  }
}

// =============================================================================
// Test 2 -- PIPELINE-barrier overlap: consumer runs while producer is
// still active, proven by an explicit release gate rather than timing
// =============================================================================

TEST(PipelineExecutorTest, PipelineBarrierConsumerOverlapsWithActiveProducer) {
  std::promise<void> release_promise;
  std::shared_future<void> release_future = release_promise.get_future().share();

  GatedTwoBatchSource producer_op{0, MakeTestBlock("b0", 4), MakeTestBlock("b1", 4),
                                  release_future};
  WaitableSink consumer_op{1};

  // Wires a real BatchHolder between the two default ports; MetaPipeline
  // is deliberately not used here (a plain edge like this always fuses --
  // see MetaPipelineTest.LinearPlanFusesIntoOnePipeline), so the two
  // pipelines are hand-assembled to force the Pipeline-barrier split this
  // test needs while still exercising ClaimNextBatch's real holder-pull
  // path (Pipeline::PublishOutput's AddToOutput call on the producer side).
  {
    GraphBuilder gb;
    gb.Chain(producer_op, consumer_op);
    ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
    graph->Materialize();
  }

  auto producer_pipeline = std::make_unique<Pipeline>(0);
  producer_pipeline->PrependOperator(&producer_op);

  auto consumer_pipeline = std::make_unique<Pipeline>(1);
  consumer_pipeline->PrependOperator(&consumer_op);
  consumer_pipeline->AddProducer(producer_pipeline.get(), BarrierType::Pipeline);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(producer_pipeline));
  pipelines.push_back(std::move(consumer_pipeline));

  PipelineExecutor executor{2};
  std::promise<Status> result_promise;
  std::future<Status> result_future = result_promise.get_future();
  // A plain std::thread, not std::async: a future obtained from std::async
  // blocks in its destructor until the task completes, so if Execute()
  // ever regressed to a real hang, a bounded wait_for() below would
  // report the failure correctly but the test binary would then hang
  // anyway when execute_future went out of scope. A std::promise/future
  // pair has no such destructor obligation, so a timeout can genuinely
  // fail fast; detach() on that path deliberately leaks the thread rather
  // than block anything further.
  std::thread execute_thread(
      [&] { result_promise.set_value(executor.Execute(std::move(pipelines))); });

  // The consumer must receive the first batch without the producer ever
  // reaching its second GetNextBatch() call -- that call is blocked on
  // release_future, so this can only pass if the two pipelines are
  // genuinely running concurrently, not producer-then-consumer.
  bool got_first_batch = consumer_op.WaitForAtLeast(1, std::chrono::seconds(5));

  // Always release the producer before any assertion can return early,
  // so a failed WaitForAtLeast above doesn't also leave Execute() blocked
  // on the gate for no reason.
  release_promise.set_value();
  EXPECT_TRUE(got_first_batch)
      << "consumer never received a batch while the producer was still blocked";

  if (result_future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    execute_thread.detach();
    FAIL() << "PipelineExecutor::Execute did not finish after the producer was released";
  }
  execute_thread.join();
  auto status = result_future.get();

  ASSERT_TRUE(status.has_value()) << status.error().ToString();
  EXPECT_EQ(consumer_op.received_count(), 2U);
}

// =============================================================================
// Test 3 -- completion-race stress: many independent pipelines finishing
// concurrently must all be accounted for exactly once, with no hang
// =============================================================================

TEST(PipelineExecutorTest, ManyIndependentPipelinesAllCompleteExactlyOnce) {
  constexpr std::size_t kPipelineCount = 12;
  constexpr std::size_t kBatchesPerPipeline = 40;

  std::vector<std::unique_ptr<VectorSourceOperator>> sources;
  std::vector<std::unique_ptr<CollectingSink>> sinks;
  std::vector<PipelinePtr> pipelines;

  for (std::size_t i = 0; i < kPipelineCount; ++i) {
    auto source = std::make_unique<VectorSourceOperator>(static_cast<std::uint32_t>(i * 2),
                                                         kBatchesPerPipeline);
    auto sink = std::make_unique<CollectingSink>(static_cast<std::uint32_t>(i * 2 + 1));

    auto pipeline = std::make_unique<Pipeline>(i);
    pipeline->PrependOperator(sink.get());
    pipeline->PrependOperator(source.get());

    sources.push_back(std::move(source));
    sinks.push_back(std::move(sink));
    pipelines.push_back(std::move(pipeline));
  }

  PipelineExecutor executor{4};
  auto status = executor.Execute(std::move(pipelines));
  ASSERT_TRUE(status.has_value()) << status.error().ToString();

  for (const auto& sink : sinks) {
    EXPECT_EQ(sink->received_count(), kBatchesPerPipeline);
  }
}

// =============================================================================
// Test 4 -- error propagation: a failing pipeline's error surfaces, and a
// pipeline gated behind it via a Full barrier never runs
// =============================================================================

TEST(PipelineExecutorTest, ErrorPropagatesAndStopsDownstreamScheduling) {
  VectorSourceOperator source{0, 3};
  FailingOperator failing{1};
  CollectingSink downstream_sink{2};

  auto failing_pipeline = std::make_unique<Pipeline>(0);
  failing_pipeline->PrependOperator(&failing);
  failing_pipeline->PrependOperator(&source);

  auto downstream_pipeline = std::make_unique<Pipeline>(1);
  downstream_pipeline->PrependOperator(&downstream_sink);
  downstream_pipeline->AddProducer(failing_pipeline.get(), BarrierType::Full);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(failing_pipeline));
  pipelines.push_back(std::move(downstream_pipeline));

  PipelineExecutor executor{2};
  auto status = executor.Execute(std::move(pipelines));

  ASSERT_FALSE(status.has_value());
  // Gated behind the failing pipeline's Full barrier: it can only become
  // runnable after the producer finishes, which it never does.
  EXPECT_EQ(downstream_sink.received_count(), 0U);
}

// =============================================================================
// Test 4b -- a Full-barrier cycle among a *subset* of pipelines must fail,
// not hang, even though another pipeline is independently runnable.
//
// MetaPipeline::Build rejects cycles at the operator level, but pipelines
// can also be hand-assembled directly (as in this file), bypassing that
// check entirely -- the executor itself must detect the resulting
// deadlock rather than leave every worker parked on schedule_cv_ forever.
// =============================================================================

TEST(PipelineExecutorTest, FullBarrierCycleAmongSubsetOfPipelinesFailsInsteadOfHanging) {
  VectorSourceOperator runnable_source{0, 2};
  CollectingSink runnable_sink{1};
  auto runnable_pipeline = std::make_unique<Pipeline>(0);
  runnable_pipeline->PrependOperator(&runnable_sink);
  runnable_pipeline->PrependOperator(&runnable_source);

  VectorSourceOperator cycle_source_a{2, 1};
  CollectingSink cycle_sink_a{3};
  auto cycle_pipeline_a = std::make_unique<Pipeline>(1);
  cycle_pipeline_a->PrependOperator(&cycle_sink_a);
  cycle_pipeline_a->PrependOperator(&cycle_source_a);

  VectorSourceOperator cycle_source_b{4, 1};
  CollectingSink cycle_sink_b{5};
  auto cycle_pipeline_b = std::make_unique<Pipeline>(2);
  cycle_pipeline_b->PrependOperator(&cycle_sink_b);
  cycle_pipeline_b->PrependOperator(&cycle_source_b);

  // Mutual Full dependency: neither can ever satisfy "all
  // FullBarrierProducers() finished", so neither is ever runnable.
  cycle_pipeline_a->AddProducer(cycle_pipeline_b.get(), BarrierType::Full);
  cycle_pipeline_b->AddProducer(cycle_pipeline_a.get(), BarrierType::Full);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(runnable_pipeline));
  pipelines.push_back(std::move(cycle_pipeline_a));
  pipelines.push_back(std::move(cycle_pipeline_b));

  PipelineExecutor executor{4};
  std::promise<Status> result_promise;
  std::future<Status> result_future = result_promise.get_future();
  std::thread execute_thread(
      [&] { result_promise.set_value(executor.Execute(std::move(pipelines))); });

  if (result_future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
    execute_thread.detach();
    FAIL() << "PipelineExecutor::Execute deadlocked on a Full-barrier cycle "
              "among a subset of pipelines instead of failing";
    return;
  }
  execute_thread.join();
  auto status = result_future.get();

  ASSERT_FALSE(status.has_value());
  // The independent pipeline is unaffected by the cycle elsewhere.
  EXPECT_EQ(runnable_sink.received_count(), 2U);
}

// =============================================================================
// Test 5 -- worker-pool bound: more ready pipelines than workers still run
// all of them correctly
// =============================================================================

TEST(PipelineExecutorTest, WorkerPoolSmallerThanPipelineCountStillCompletesAll) {
  constexpr std::size_t kPipelineCount = 6;
  constexpr std::size_t kBatchesPerPipeline = 5;

  std::vector<std::unique_ptr<VectorSourceOperator>> sources;
  std::vector<std::unique_ptr<CollectingSink>> sinks;
  std::vector<PipelinePtr> pipelines;

  for (std::size_t i = 0; i < kPipelineCount; ++i) {
    auto source = std::make_unique<VectorSourceOperator>(static_cast<std::uint32_t>(i * 2),
                                                         kBatchesPerPipeline);
    auto sink = std::make_unique<CollectingSink>(static_cast<std::uint32_t>(i * 2 + 1));

    auto pipeline = std::make_unique<Pipeline>(i);
    pipeline->PrependOperator(sink.get());
    pipeline->PrependOperator(source.get());

    sources.push_back(std::move(source));
    sinks.push_back(std::move(sink));
    pipelines.push_back(std::move(pipeline));
  }

  PipelineExecutor executor{2};  // Fewer workers than pipelines.
  auto status = executor.Execute(std::move(pipelines));
  ASSERT_TRUE(status.has_value()) << status.error().ToString();

  for (const auto& sink : sinks) {
    EXPECT_EQ(sink->received_count(), kBatchesPerPipeline);
  }
}

// =============================================================================
// Test 6 -- Graph::Execute() end-to-end smoke test
// =============================================================================

TEST(PipelineExecutorTest, GraphExecuteRunsALinearPlan) {
  EventLog log;

  LoggingSourceOperator scan{0, {MakeTestBlock("b0", 4), MakeTestBlock("b1", 4)}, log, "source"};
  LoggingStreamOperator filter{1, log, "filter"};
  LoggingCollectingSink collector{2, log, "collector"};

  GraphBuilder gb;
  gb.Chain(scan, filter, collector);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto status = graph->Execute();
  ASSERT_TRUE(status.has_value()) << status.error().ToString();

  EXPECT_EQ(filter.processed_count(), 2U);
  EXPECT_EQ(collector.received_count(), 2U);
}

// =============================================================================
// Test 7 -- a holder-fed first operator must run its own ProcessBatch.
//
// Regression test: DrainPipeline used to call RunTask with the default
// start_index=1 unconditionally, which is only correct when operators()[0]
// itself produced the batch (a role-based source). When the batch was
// claimed from a cross-pipeline holder instead, the first operator never
// saw it -- its transform was silently dropped in any multi-operator
// pipeline beginning at an explicit Full/partitioned break.
// =============================================================================

TEST(PipelineExecutorTest, HolderFedFirstOperatorRunsItsOwnProcessBatch) {
  constexpr std::size_t kBatchCount = 3;
  EventLog log;

  VectorSourceOperator producer_op{0, kBatchCount};
  LoggingStreamOperator holder_fed_op{1, log, "holder_fed"};
  LoggingCollectingSink sink_op{2, log, "sink"};

  // Wires the real BatchHolder the consumer pipeline claims from
  // (producer_op's default output port -> holder_fed_op's default input).
  {
    GraphBuilder gb;
    gb.Chain(producer_op, holder_fed_op);
    ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
    graph->Materialize();
  }

  auto producer_pipeline = std::make_unique<Pipeline>(0);
  producer_pipeline->PrependOperator(&producer_op);

  // Two-operator consumer pipeline whose FIRST operator is holder-fed:
  // exactly the shape where start_index=1 used to skip holder_fed_op.
  auto consumer_pipeline = std::make_unique<Pipeline>(1);
  consumer_pipeline->PrependOperator(&sink_op);
  consumer_pipeline->PrependOperator(&holder_fed_op);
  consumer_pipeline->AddProducer(producer_pipeline.get(), BarrierType::Pipeline);

  std::vector<PipelinePtr> pipelines;
  pipelines.push_back(std::move(producer_pipeline));  // producer first (ordering invariant)
  pipelines.push_back(std::move(consumer_pipeline));

  PipelineExecutor executor{2};
  auto status = executor.Execute(std::move(pipelines));
  ASSERT_TRUE(status.has_value()) << status.error().ToString();

  EXPECT_EQ(holder_fed_op.processed_count(), kBatchCount)
      << "the holder-fed first operator's ProcessBatch was skipped";
  EXPECT_EQ(sink_op.received_count(), kBatchCount);
}

}  // namespace
