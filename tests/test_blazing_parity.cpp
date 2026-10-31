// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_blazing_parity.cpp
/// @brief Phase 2.6 — BlazingSQL edge/kernel parity, CPU-only.
///
/// Covers the three stages from tasks/todo.md:
///   E1 — kNonWaiting holder (BlazingSQL NonWaitingCacheMachine): pre-filled
///        single-shot handoffs; empty == drained; builder validation.
///   E2 — ConcatenatingUpTo(bytes): chunked concatenation with a byte cap
///        (BlazingSQL mature concat cap / Sirius _concat_batch_bytes),
///        stats-tunable via the policy chain.
///   E3 — FOR_EACH lanes end-to-end: a Partitioned(N) edge fans out over
///        output_0…N-1 (round-robin publish) and the consumer drains
///        input_0…N-1 sequentially — the partition→merge cache shape.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/prepared_query.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

namespace {

[[nodiscard]] DataBlockPtr MakeTestBlock(const std::string& block_id, std::size_t rows) {
  auto batch = quest::testing::MakeTestRecordBatch(rows);
  return std::make_shared<HostMemoryBlock>(block_id, std::move(batch));
}

/// @brief Source yielding N synthetic batches of `rows` rows each.
class VectorSource : public Operator {
 public:
  VectorSource(std::uint32_t operator_id, std::size_t batch_count, std::size_t rows)
      : Operator(operator_id, "vector_source", OperatorType::TableScan) {
    for (std::size_t i = 0; i < batch_count; ++i) {
      batches_.push_back(MakeTestBlock("b" + std::to_string(i), rows));
    }
  }

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (next_ >= batches_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    return std::optional<DataBlockPtr>{batches_[next_++]};
  }

 private:
  std::vector<DataBlockPtr> batches_;
  std::size_t next_{0};
};

/// @brief Source that yields EOS immediately (the "external pre-fill" shape:
/// the edge's data comes from outside the graph, not from this operator).
class EmptySource : public Operator {
 public:
  explicit EmptySource(std::uint32_t operator_id)
      : Operator(operator_id, "empty_source", OperatorType::TableScan) {}
  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    return std::optional<DataBlockPtr>{std::nullopt};
  }
};

/// @brief Contract-free collecting sink (no Full InputEdgeContract, unlike
/// CollectorOperator — lets tests control the barrier explicitly).
class CollectingSink : public Operator {
 public:
  explicit CollectingSink(std::uint32_t operator_id)
      : Operator(operator_id, "collecting_sink", OperatorType::Output) {}
  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    if (batch) {
      total_rows_ += batch->num_rows();
      ++batches_received_;
    }
    return Status::OK();
  }

  [[nodiscard]] std::size_t total_rows() const noexcept { return total_rows_; }
  [[nodiscard]] std::size_t batches_received() const noexcept { return batches_received_; }

 private:
  std::size_t total_rows_{0};
  std::size_t batches_received_{0};
};

}  // namespace

// =============================================================================
// E1 — kNonWaiting holder
// =============================================================================

TEST(BlazingParityTest, NonWaitingHolderIsPreFilledSingleShot) {
  NonWaitingBatchHolder holder{1};

  ASSERT_OK(holder.Add(MakeTestBlock("a", 3)));
  ASSERT_OK(holder.Add(MakeTestBlock("b", 2)));

  EXPECT_TRUE(holder.HasNext());
  EXPECT_FALSE(holder.IsFinished());  // has data -> not drained

  ASSERT_OK_AND_ASSIGN(auto first, holder.Pull());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)->num_rows(), 3U);
  ASSERT_OK_AND_ASSIGN(auto second, holder.Pull());
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ((*second)->num_rows(), 2U);

  // Empty == drained: no finished-latch needed, Pull never blocks.
  EXPECT_TRUE(holder.IsFinished());
  EXPECT_FALSE(holder.HasNext());
  auto eos = holder.Pull();
  ASSERT_TRUE(eos.has_value());
  EXPECT_FALSE(eos->has_value());
}

TEST(BlazingParityTest, NonWaitingEdgeRejectsFlowControlAndFull) {
  {
    EmptySource source{0};
    CollectingSink sink{1};
    GraphBuilder gb;
    gb.Edge(source, sink).NonWaiting().FlowControl(4);
    auto result = std::move(gb).Build();
    ASSERT_FALSE(result.has_value());  // no producer to throttle
  }
  {
    EmptySource source{0};
    CollectingSink sink{1};
    GraphBuilder gb;
    gb.Edge(source, sink).NonWaiting().Full();
    auto result = std::move(gb).Build();
    ASSERT_FALSE(result.has_value());  // no producer pipeline to wait on
  }
}

TEST(BlazingParityTest, NonWaitingEdgeEndToEndWithPreFill) {
  EmptySource source{0};  // the graph's producer contributes nothing
  CollectingSink sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).NonWaiting().Named("prefilled");
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));

  // The BlazingSQL direct-execution pattern: pre-load the cache, then run.
  auto holders = query.holders();
  ASSERT_EQ(holders.size(), 1U);
  ASSERT_EQ(holders[0]->type(), BatchHolderType::kNonWaiting);
  ASSERT_OK(holders[0]->Add(MakeTestBlock("pre_a", 5)));
  ASSERT_OK(holders[0]->Add(MakeTestBlock("pre_b", 7)));

  ASSERT_OK(query.Execute());

  EXPECT_EQ(sink.batches_received(), 2U);
  EXPECT_EQ(sink.total_rows(), 12U);
}

// =============================================================================
// E2 — ConcatenatingUpTo (chunked concatenation)
// =============================================================================

TEST(BlazingParityTest, ConcatenatingUpToServesChunksUnderCap) {
  // MakeTestRecordBatch(10) -> one int32 column, 10 rows -> 80 bytes by the
  // current size estimate. Cap = 160 bytes -> two blocks per chunk.
  BatchHolderConfig config{};
  config.concat_bytes_threshold = 160;
  ConcatenatingBatchHolder holder{1, config};

  for (int i = 0; i < 4; ++i) {
    ASSERT_OK(holder.Add(MakeTestBlock("c" + std::to_string(i), 10)));
  }
  holder.Finish();

  ASSERT_OK_AND_ASSIGN(auto chunk1, holder.Pull());
  ASSERT_TRUE(chunk1.has_value());
  EXPECT_EQ((*chunk1)->num_rows(), 20U);  // two 10-row blocks merged
  EXPECT_TRUE(holder.HasNext());

  ASSERT_OK_AND_ASSIGN(auto chunk2, holder.Pull());
  ASSERT_TRUE(chunk2.has_value());
  EXPECT_EQ((*chunk2)->num_rows(), 20U);

  // Drained: same EOS protocol as the simple holder.
  EXPECT_TRUE(holder.IsFinished());
  auto eos = holder.Pull();
  ASSERT_TRUE(eos.has_value());
  EXPECT_FALSE(eos->has_value());
}

TEST(BlazingParityTest, ConcatenatingUpToOversizedBlockStillFlows) {
  BatchHolderConfig config{};
  config.concat_bytes_threshold = 8;  // smaller than any single block
  ConcatenatingBatchHolder holder{1, config};

  ASSERT_OK(holder.Add(MakeTestBlock("big", 10)));  // ~80 bytes > cap
  holder.Finish();

  // The oversized-batch rule: a chunk always contains at least one block.
  ASSERT_OK_AND_ASSIGN(auto chunk, holder.Pull());
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ((*chunk)->num_rows(), 10U);
  EXPECT_TRUE(holder.IsFinished());
}

TEST(BlazingParityTest, ThresholdZeroKeepsConcatAllBehavior) {
  ConcatenatingBatchHolder holder{1};  // concat_bytes_threshold = 0

  ASSERT_OK(holder.Add(MakeTestBlock("a", 3)));
  ASSERT_OK(holder.Add(MakeTestBlock("b", 2)));
  holder.Finish();

  ASSERT_OK_AND_ASSIGN(auto merged, holder.Pull());
  ASSERT_TRUE(merged.has_value());
  EXPECT_EQ((*merged)->num_rows(), 5U);  // everything in one block
}

TEST(BlazingParityTest, PolicySizesConcatCapFromStats) {
  VectorSource source{0, 1, 1};
  CollectingSink sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).Concatenating().Named("concat_edge");  // cap unset by author
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  // The cap is exactly the kind of knob that should be stats-bound: a policy
  // sizes it when footer stats exist, never the author at wiring time.
  graph->AddTuningPolicy([](const EdgeInfo& edge, const QueryStats& stats, const HardwareInfo&) {
    EdgeConfig cfg = edge.config;
    if (cfg.type == cache::BatchHolderType::kConcatenating) {
      cfg.concat_bytes_threshold = stats.estimated_bytes / 4;
    }
    return cfg;
  });

  QueryStats stats{.estimated_bytes = 640};
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(stats, HardwareInfo{}));

  auto edges = query.Edges();
  ASSERT_EQ(edges.size(), 1U);
  EXPECT_EQ(edges[0].config.concat_bytes_threshold, 160U);

  // And the materialized holder actually chunks at the policy-chosen cap.
  auto holders = query.holders();
  ASSERT_EQ(holders.size(), 1U);
  for (int i = 0; i < 4; ++i) {
    ASSERT_OK(holders[0]->Add(MakeTestBlock("p" + std::to_string(i), 10)));
  }
  holders[0]->Finish();
  ASSERT_OK_AND_ASSIGN(auto chunk, holders[0]->Pull());
  ASSERT_TRUE(chunk.has_value());
  EXPECT_EQ((*chunk)->num_rows(), 20U);  // 2 blocks per 160-byte chunk
}

// =============================================================================
// E3 — FOR_EACH lanes end-to-end (the partition→merge cache shape)
// =============================================================================

TEST(BlazingParityTest, ForEachLanesFanOutAndDrainEndToEnd) {
  constexpr std::size_t kBatches = 6;
  constexpr std::size_t kRows = 4;
  constexpr std::size_t kLanes = 2;

  VectorSource source{0, kBatches, kRows};
  CollectingSink sink{1};

  GraphBuilder gb;
  // Partitioned edge: producer fans out round-robin over output_0/output_1;
  // consumer drains input_0 then input_1 (sequential, merge-friendly order).
  // Full barrier keeps the test deterministic (consumer starts after the
  // producer finished, like a merge stage consuming complete lanes).
  gb.Edge(source, sink).Partitioned(kLanes).Full().Named("exchange");
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));

  // One holder per lane, BlazingSQL naming on both ports.
  auto holders = query.holders();
  ASSERT_EQ(holders.size(), kLanes);
  ASSERT_TRUE(source.output().has_port("output_0"));
  ASSERT_TRUE(source.output().has_port("output_1"));
  ASSERT_TRUE(sink.input().has_port("input_0"));
  ASSERT_TRUE(sink.input().has_port("input_1"));

  ASSERT_OK(query.Execute());

  // Nothing lost, and BOTH lanes actually carried data (round-robin).
  EXPECT_EQ(sink.batches_received(), kBatches);
  EXPECT_EQ(sink.total_rows(), kBatches * kRows);
  EXPECT_GT(holders[0]->bytes_added(), 0U);
  EXPECT_GT(holders[1]->bytes_added(), 0U);
}
