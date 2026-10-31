// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_runtime_tuning.cpp
/// @brief Phase 2.5 Stage D5 — policy chain, live soft knobs, Edges() introspection.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/edge_ref.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/operator_type.hpp"
#include "quest/taskflow/prepared_query.hpp"
#include "quest/taskflow/query_stats.hpp"
#include "support/test_utils.hpp"

namespace {

using namespace quest;
using namespace quest::taskflow;
using quest::cache::DataBlock;
using quest::cache::DataBlockPtr;
using quest::cache::HostMemoryBlock;

DataBlockPtr MakeTestData(const std::string& id, std::size_t rows) {
  auto batch = quest::testing::MakeTestRecordBatch(rows);
  return std::make_shared<HostMemoryBlock>(id, std::move(batch));
}

class MockSource : public Operator {
 public:
  explicit MockSource(std::uint32_t id) : Operator(id, "mock_source", OperatorType::TableScan) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  [[nodiscard]] Result<std::optional<std::shared_ptr<DataBlock>>> GetNextBatch() override {
    return std::optional<std::shared_ptr<DataBlock>>{std::nullopt};
  }
};

class MockSink : public Operator {
 public:
  explicit MockSink(std::uint32_t id) : Operator(id, "mock_sink", OperatorType::Output) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(std::shared_ptr<DataBlock> /*batch*/) override { return Status::OK(); }
};

class MockMid : public Operator {
 public:
  explicit MockMid(std::uint32_t id) : Operator(id, "mock_mid", OperatorType::Filter) {}
};

// ---------------------------------------------------------------------------
// PolicyOrdering: last write wins; second policy sees first's output
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, PolicyOrdering) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).FlowControl(8, 1000).Named("e0");
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  graph->AddTuningPolicy([](const EdgeInfo& e, const QueryStats&, const HardwareInfo&) {
    EdgeConfig cfg = e.config;
    cfg.flow_control_batches_threshold = 16;  // refine from author 8
    cfg.flow_control_bytes_threshold = 2000;
    return cfg;
  });
  graph->AddTuningPolicy([](const EdgeInfo& e, const QueryStats&, const HardwareInfo&) {
    // Must see first policy's batches=16; last write wins for bytes.
    EXPECT_EQ(e.config.flow_control_batches_threshold, 16U);
    EdgeConfig cfg = e.config;
    cfg.flow_control_bytes_threshold = 4000;  // overrides first's 2000
    return cfg;
  });

  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));
  auto edges = query.Edges();
  ASSERT_EQ(edges.size(), 1U);
  EXPECT_EQ(edges[0].config.flow_control_batches_threshold, 16U);
  EXPECT_EQ(edges[0].config.flow_control_bytes_threshold, 4000U);

  // Built view still shows author config (policies don't mutate Graph specs).
  auto built = graph->Edges();
  ASSERT_EQ(built.size(), 1U);
  EXPECT_EQ(built[0].config.flow_control_batches_threshold, 8U);
  EXPECT_EQ(built[0].config.flow_control_bytes_threshold, 1000U);
}

// ---------------------------------------------------------------------------
// RePrepareIndependence: same Graph, different stats → different FC
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, RePrepareIndependence) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Chain(source, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  graph->AddTuningPolicy([](const EdgeInfo& e, const QueryStats& s, const HardwareInfo&) {
    EdgeConfig cfg = e.config;
    cfg.flow_control_batches_threshold = std::max<std::size_t>(1, s.estimated_bytes / 100);
    cfg.flow_control_bytes_threshold = s.estimated_bytes;
    return cfg;
  });

  QueryStats stats_a{.estimated_bytes = 1000};
  QueryStats stats_b{.estimated_bytes = 5000};

  ASSERT_OK_AND_ASSIGN(auto qa, graph->Prepare(stats_a, HardwareInfo{}));
  ASSERT_OK_AND_ASSIGN(auto qb, graph->Prepare(stats_b, HardwareInfo{}));

  auto ea = qa.Edges();
  auto eb = qb.Edges();
  ASSERT_EQ(ea.size(), 1U);
  ASSERT_EQ(eb.size(), 1U);

  EXPECT_EQ(ea[0].config.flow_control_batches_threshold, 10U);
  EXPECT_EQ(ea[0].config.flow_control_bytes_threshold, 1000U);
  EXPECT_EQ(eb[0].config.flow_control_batches_threshold, 50U);
  EXPECT_EQ(eb[0].config.flow_control_bytes_threshold, 5000U);

  auto ha = qa.holders();
  auto hb = qb.holders();
  ASSERT_EQ(ha.size(), 1U);
  ASSERT_EQ(hb.size(), 1U);
  EXPECT_EQ(ha[0]->flow_control_batches_threshold(), 10U);
  EXPECT_EQ(hb[0]->flow_control_batches_threshold(), 50U);
}

// ---------------------------------------------------------------------------
// LiveSetFlowControlReleasesBlockedProducer
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, LiveSetFlowControlReleasesBlockedProducer) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).FlowControl(1, kUnlimited).Named("fc");
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));

  auto edges = query.Edges();
  ASSERT_EQ(edges.size(), 1U);
  auto holders = query.holders();
  ASSERT_EQ(holders.size(), 1U);
  auto& holder = holders[0];

  // Saturate: one batch in the holder → WaitIfSaturated blocks at threshold=1.
  ASSERT_OK(holder->Add(MakeTestData("b0", 4)));

  std::atomic<bool> entered_wait{false};
  std::atomic<bool> released{false};

  auto waiter = std::async(std::launch::async, [&]() {
    entered_wait.store(true, std::memory_order_release);
    holder->WaitIfSaturated();
    released.store(true, std::memory_order_release);
  });

  // Wait until the producer thread is inside WaitIfSaturated.
  for (int i = 0; i < 200 && !entered_wait.load(std::memory_order_acquire); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(entered_wait.load());
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_FALSE(released.load(std::memory_order_acquire));

  ASSERT_OK(query.SetFlowControl(edges[0].ref, 64, kUnlimited));

  auto status = waiter.wait_for(std::chrono::seconds(2));
  ASSERT_EQ(status, std::future_status::ready);
  EXPECT_TRUE(released.load(std::memory_order_acquire));
  EXPECT_EQ(holder->flow_control_batches_threshold(), 64U);
}

// ---------------------------------------------------------------------------
// ScaleFlowControlHalvesAllEdges
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, ScaleFlowControlHalvesAllEdges) {
  MockSource source{0};
  MockMid mid{1};
  MockSink sink{2};

  GraphBuilder gb;
  gb.Edge(source, mid).FlowControl(10, 1000);
  gb.Edge(mid, sink).FlowControl(20, 2000).Named("to_sink");
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));

  ASSERT_OK(query.ScaleFlowControl(0.5));

  auto edges = query.Edges();
  ASSERT_EQ(edges.size(), 2U);
  EXPECT_EQ(edges[0].config.flow_control_batches_threshold, 5U);
  EXPECT_EQ(edges[0].config.flow_control_bytes_threshold, 500U);
  EXPECT_EQ(edges[1].config.flow_control_batches_threshold, 10U);
  EXPECT_EQ(edges[1].config.flow_control_bytes_threshold, 1000U);

  auto holders = query.holders();
  ASSERT_EQ(holders.size(), 2U);
  EXPECT_EQ(holders[0]->flow_control_batches_threshold(), 5U);
  EXPECT_EQ(holders[1]->flow_control_batches_threshold(), 10U);
}

// ---------------------------------------------------------------------------
// SetMaxInFlightTasksReturnsInvalid
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, SetMaxInFlightTasksReturnsInvalid) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Chain(source, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));

  auto status = query.SetMaxInFlightTasks(0, 4);
  EXPECT_FALSE(status.ok());
  EXPECT_NE(status.error().message().find("reserved until phase 3"), std::string::npos);
}

// ---------------------------------------------------------------------------
// NamedEdgeTuneAndEdgesIntrospection
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, NamedEdgeTuneAndEdgesIntrospection) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).Named("into_sink").FlowControl(4, 512);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto built = graph->Edges();
  ASSERT_EQ(built.size(), 1U);
  EXPECT_EQ(built[0].name, "into_sink");
  EXPECT_FALSE(built[0].synthesized);
  EXPECT_EQ(built[0].source_type, OperatorType::TableScan);
  EXPECT_EQ(built[0].target_type, OperatorType::Output);
  EXPECT_EQ(built[0].ref.source_id, 0);
  EXPECT_EQ(built[0].ref.target_id, 1);

  ASSERT_OK_AND_ASSIGN(auto tuner, graph->Tune("into_sink"));
  tuner.FlowControl(32, 4096).Spill(SpillPolicy::Never);

  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));
  auto prepared = query.Edges();
  ASSERT_EQ(prepared.size(), 1U);
  EXPECT_EQ(prepared[0].name, "into_sink");
  EXPECT_EQ(prepared[0].config.flow_control_batches_threshold, 32U);
  EXPECT_EQ(prepared[0].config.flow_control_bytes_threshold, 4096U);
  EXPECT_EQ(prepared[0].config.spill_policy, SpillPolicy::Never);
}

// ---------------------------------------------------------------------------
// SpillPolicyNeverMapsToAutoSpillFalse
// ---------------------------------------------------------------------------

TEST(RuntimeTuningTest, SpillPolicyNeverMapsToAutoSpillFalse) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).Spill(SpillPolicy::Never);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(QueryStats{}, HardwareInfo{}));

  auto holders = query.holders();
  ASSERT_EQ(holders.size(), 1U);
  EXPECT_FALSE(holders[0]->auto_spill());

  // Eligible via live SetSpillPolicy flips auto_spill on.
  ASSERT_OK(query.SetSpillPolicy(query.Edges()[0].ref, SpillPolicy::Eligible));
  EXPECT_TRUE(holders[0]->auto_spill());

  ASSERT_OK(query.SetSpillPolicy(query.Edges()[0].ref, SpillPolicy::Never));
  EXPECT_FALSE(holders[0]->auto_spill());
}

}  // namespace
