// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_operator_roles.cpp
/// @brief Tests for the pipeline role virtuals on Operator
///        (execute / source / sink), Stage 2 of
///        docs/dev/pipeline-execution-plan.md.
///
/// The three test doubles defined here (VectorSourceOperator,
/// CountingStreamOperator, AccumulatingSinkOperator) are the building blocks
/// for the Stage 3/4 pipeline shape and execution tests.

#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/operator_type.hpp"
#include "support/test_utils.hpp"

namespace quest::taskflow {
namespace {

using cache::DataBlockPtr;
using cache::HostMemoryBlock;

[[nodiscard]] DataBlockPtr MakeTestBlock(const std::string& block_id, std::size_t rows) {
  auto batch = quest::testing::MakeTestRecordBatch(rows);
  return std::make_shared<HostMemoryBlock>(block_id, std::move(batch));
}

// =============================================================================
// Test Doubles
// =============================================================================

/// @brief Source double: yields a fixed list of batches, then end-of-stream.
class VectorSourceOperator : public Operator {
 public:
  VectorSourceOperator(std::uint32_t id, std::vector<DataBlockPtr> batches)
      : Operator(id, "vector_source", OperatorType::TableScan),
        pending_(batches.begin(), batches.end()) {}

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (pending_.empty()) {
      return std::optional<DataBlockPtr>{std::nullopt};  // EOS is a value
    }
    auto batch = std::move(pending_.front());
    pending_.pop_front();
    return std::optional<DataBlockPtr>{std::move(batch)};
  }

 private:
  std::deque<DataBlockPtr> pending_;
};

/// @brief Streaming double: passthrough that counts batches it processed.
class CountingStreamOperator : public Operator {
 public:
  explicit CountingStreamOperator(std::uint32_t id)
      : Operator(id, "counting_stream", OperatorType::Filter) {}

  [[nodiscard]] Result<DataBlockPtr> ProcessBatch(DataBlockPtr input) override {
    ++processed_count_;
    return input;
  }

  [[nodiscard]] std::size_t processed_count() const noexcept { return processed_count_; }

 private:
  std::size_t processed_count_{0};
};

/// @brief Blocking-sink double: accumulates batches via Sink(), and after
///        FinalizeSink() flips to source mode, re-emitting what it collected.
///        Stand-in for aggregate/sort until real blocking operators exist.
class AccumulatingSinkOperator : public Operator {
 public:
  explicit AccumulatingSinkOperator(std::uint32_t id)
      : Operator(id, "accumulating_sink", OperatorType::ComputeAggregate) {}

  // Sink role -----------------------------------------------------------
  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    if (finalized_) {
      return Error::Invalid("Sink called after FinalizeSink");
    }
    accumulated_.push_back(std::move(batch));
    return {};
  }

  [[nodiscard]] Status FinalizeSink() override {
    if (finalized_) {
      return Error::Invalid("FinalizeSink called twice");
    }
    finalized_ = true;
    return {};
  }

  // Source role (valid only after finalization) --------------------------
  [[nodiscard]] bool IsSource() const noexcept override { return finalized_; }

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (!finalized_) {
      return Error::Invalid("GetNextBatch before FinalizeSink");
    }
    if (emit_index_ >= accumulated_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};  // EOS
    }
    return std::optional<DataBlockPtr>{accumulated_[emit_index_++]};
  }

  [[nodiscard]] std::size_t accumulated_count() const noexcept { return accumulated_.size(); }
  [[nodiscard]] bool finalized() const noexcept { return finalized_; }

 private:
  std::vector<DataBlockPtr> accumulated_;
  std::size_t emit_index_{0};
  bool finalized_{false};
};

// =============================================================================
// Default role behavior on the Operator base
// =============================================================================

class DefaultRolesOperator : public Operator {
 public:
  DefaultRolesOperator(std::uint32_t id, OperatorType type) : Operator(id, "default_roles", type) {}
};

TEST(OperatorRolesTest, DefaultProcessBatchIsPassthrough) {
  DefaultRolesOperator op{0, OperatorType::Output};
  auto block = MakeTestBlock("b0", 8);

  auto result = op.ProcessBatch(block);
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value().get(), block.get());  // same object, not a copy
}

TEST(OperatorRolesTest, DefaultGetNextBatchIsAnError) {
  DefaultRolesOperator op{0, OperatorType::Filter};
  auto result = op.GetNextBatch();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Invalid);
}

TEST(OperatorRolesTest, DefaultSinkIsAnError) {
  DefaultRolesOperator op{0, OperatorType::Filter};
  auto status = op.Sink(MakeTestBlock("b0", 4));
  EXPECT_FALSE(status.has_value());
}

TEST(OperatorRolesTest, DefaultFinalizeSinkIsOkNoOp) {
  DefaultRolesOperator op{0, OperatorType::Filter};
  EXPECT_TRUE(op.FinalizeSink().has_value());
}

TEST(OperatorRolesTest, IsSourceConsultsTypeByDefault) {
  DefaultRolesOperator scan{0, OperatorType::TableScan};
  DefaultRolesOperator filter{1, OperatorType::Filter};
  EXPECT_TRUE(scan.IsSource());
  EXPECT_FALSE(filter.IsSource());
}

TEST(OperatorRolesTest, IsSinkDefaultsToIsBlocking) {
  DefaultRolesOperator aggregate{0, OperatorType::ComputeAggregate};
  DefaultRolesOperator filter{1, OperatorType::Filter};
  EXPECT_TRUE(aggregate.IsSink());
  EXPECT_FALSE(filter.IsSink());
}

// =============================================================================
// VectorSourceOperator
// =============================================================================

TEST(OperatorRolesTest, VectorSourceYieldsAllBatchesThenEos) {
  auto b0 = MakeTestBlock("b0", 4);
  auto b1 = MakeTestBlock("b1", 8);
  VectorSourceOperator source{0, {b0, b1}};

  auto first = source.GetNextBatch();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first.value().has_value());
  EXPECT_EQ(first.value().value().get(), b0.get());

  auto second = source.GetNextBatch();
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second.value().has_value());
  EXPECT_EQ(second.value().value().get(), b1.get());

  // EOS is an engaged Result holding nullopt — not an error.
  auto eos = source.GetNextBatch();
  ASSERT_TRUE(eos.has_value());
  EXPECT_FALSE(eos.value().has_value());

  // EOS is stable: asking again keeps returning nullopt.
  auto eos_again = source.GetNextBatch();
  ASSERT_TRUE(eos_again.has_value());
  EXPECT_FALSE(eos_again.value().has_value());
}

TEST(OperatorRolesTest, EmptyVectorSourceIsImmediatelyExhausted) {
  VectorSourceOperator source{0, {}};
  auto eos = source.GetNextBatch();
  ASSERT_TRUE(eos.has_value());
  EXPECT_FALSE(eos.value().has_value());
}

// =============================================================================
// AccumulatingSinkOperator (the mock blocking operator)
// =============================================================================

TEST(OperatorRolesTest, AccumulatingSinkCollectsThenReEmitsAfterFinalize) {
  AccumulatingSinkOperator sink{0};
  EXPECT_TRUE(sink.IsSink());     // ComputeAggregate is blocking
  EXPECT_FALSE(sink.IsSource());  // not a source until finalized

  auto b0 = MakeTestBlock("b0", 4);
  auto b1 = MakeTestBlock("b1", 8);
  ASSERT_TRUE(sink.Sink(b0).has_value());
  ASSERT_TRUE(sink.Sink(b1).has_value());
  EXPECT_EQ(sink.accumulated_count(), 2U);

  // Source role is rejected before finalization.
  EXPECT_FALSE(sink.GetNextBatch().has_value());

  ASSERT_TRUE(sink.FinalizeSink().has_value());
  EXPECT_TRUE(sink.finalized());
  EXPECT_TRUE(sink.IsSource());  // sink of one pipeline, source of the next

  auto first = sink.GetNextBatch();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first.value().has_value());
  EXPECT_EQ(first.value().value().get(), b0.get());

  auto second = sink.GetNextBatch();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second.value().value().get(), b1.get());

  auto eos = sink.GetNextBatch();
  ASSERT_TRUE(eos.has_value());
  EXPECT_FALSE(eos.value().has_value());
}

TEST(OperatorRolesTest, AccumulatingSinkRejectsProtocolViolations) {
  AccumulatingSinkOperator sink{0};
  ASSERT_TRUE(sink.FinalizeSink().has_value());

  // Sink after finalize: the producer pipeline already finished.
  EXPECT_FALSE(sink.Sink(MakeTestBlock("late", 4)).has_value());
  // Double finalize: FinalizeSink fires exactly once per pipeline.
  EXPECT_FALSE(sink.FinalizeSink().has_value());
}

// =============================================================================
// Manual mini-pipeline: source -> stream -> sink -> finalize -> drain
// (hand-rolled version of what Pipeline::RunTask automates in Stage 3)
// =============================================================================

TEST(OperatorRolesTest, ManualPushLoopThroughAllThreeRoles) {
  auto b0 = MakeTestBlock("b0", 4);
  auto b1 = MakeTestBlock("b1", 8);
  auto b2 = MakeTestBlock("b2", 2);

  VectorSourceOperator source{0, {b0, b1, b2}};
  CountingStreamOperator stream{1};
  AccumulatingSinkOperator sink{2};

  // Producer pipeline: push every source batch through the streaming
  // operator into the sink.
  while (true) {
    auto next = source.GetNextBatch();
    ASSERT_TRUE(next.has_value());
    if (!next.value().has_value()) {
      break;  // EOS
    }
    auto processed = stream.ProcessBatch(std::move(next.value().value()));
    ASSERT_TRUE(processed.has_value());
    ASSERT_TRUE(sink.Sink(std::move(processed.value())).has_value());
  }
  ASSERT_TRUE(sink.FinalizeSink().has_value());

  EXPECT_EQ(stream.processed_count(), 3U);
  EXPECT_EQ(sink.accumulated_count(), 3U);

  // Consumer pipeline: drain the finalized sink as a source and verify
  // batch identity survived the whole trip untouched.
  std::vector<DataBlockPtr> drained;
  while (true) {
    auto next = sink.GetNextBatch();
    ASSERT_TRUE(next.has_value());
    if (!next.value().has_value()) {
      break;
    }
    drained.push_back(std::move(next.value().value()));
  }
  ASSERT_EQ(drained.size(), 3U);
  EXPECT_EQ(drained[0].get(), b0.get());
  EXPECT_EQ(drained[1].get(), b1.get());
  EXPECT_EQ(drained[2].get(), b2.get());
}

}  // namespace
}  // namespace quest::taskflow
