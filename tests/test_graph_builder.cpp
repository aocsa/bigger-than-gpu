// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/operator_type.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/query_stats.hpp"
#include "support/test_utils.hpp"

namespace {

using namespace quest;
using namespace quest::taskflow;
using quest::cache::DataBlock;

// =============================================================================
// Minimal test operators
// =============================================================================

/// Minimal source — produces no data; just a placeholder for graph topology tests.
class TestSource : public Operator {
 public:
  explicit TestSource(std::uint32_t id) : Operator(id, "TestSource", OperatorType::TableScan) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  [[nodiscard]] Result<std::optional<std::shared_ptr<DataBlock>>> GetNextBatch() override {
    return std::optional<std::shared_ptr<DataBlock>>{std::nullopt};
  }
};

/// Minimal passthrough operator (filter, project, etc.).
class TestOp : public Operator {
 public:
  explicit TestOp(std::uint32_t id) : Operator(id, "TestOp", OperatorType::Filter) {}
};

/// Operator with declared named ports (simulates a HashJoin).
class TestJoinOp : public Operator {
 public:
  explicit TestJoinOp(std::uint32_t id) : Operator(id, "TestJoin", OperatorType::Filter) {}

  [[nodiscard]] OperatorPortPair probe() { return {this, "probe"}; }
  [[nodiscard]] OperatorPortPair build() { return {this, "build"}; }

  [[nodiscard]] std::vector<std::string_view> DeclaredInputPorts() const override {
    return {"probe", "build"};
  }

  [[nodiscard]] EdgeConfig InputEdgeContract(std::string_view port) const override {
    if (port == "build") {
      return EdgeConfig::Concatenating();  // build side: Concatenating + Full
    }
    return {};  // probe side: streaming
  }
};

// =============================================================================
// §8.1 — Linear chain with collector (Full inferred from contract)
// =============================================================================

TEST(GraphBuilderTest, LinearChainBuildSucceeds) {
  TestSource scan{0};
  TestOp filter{1};
  TestOp project{2};
  CollectorOperator collector{3};

  GraphBuilder gb;
  gb.Chain(scan, filter, project, collector);
  auto result = std::move(gb).Build();

  ASSERT_TRUE(result.has_value()) << result.error().ToString();
  auto graph = std::move(*result);
  ASSERT_NE(graph, nullptr);
  EXPECT_EQ(graph->GetNodeCount(), 4U);
}

TEST(GraphBuilderTest, LinearChainFullInferredFromCollectorContract) {
  TestSource scan{0};
  CollectorOperator collector{1};

  GraphBuilder gb;
  gb.Chain(scan, collector);
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  auto& specs = result.value()->edge_specs();
  ASSERT_EQ(specs.size(), 1U);
  EXPECT_EQ(specs[0].config.barrier, BarrierType::Full);
}

TEST(GraphBuilderTest, ChainLastEdgeRuleAppliesFullToLastEdgeOnly) {
  // Chain(a, b, c).Full() → only b→c gets Full; a→b stays Streaming.
  TestSource scan{0};
  TestOp filter{1};
  CollectorOperator collector{2};

  GraphBuilder gb;
  gb.Chain(scan, filter, collector).Full().Named("into_collector");
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  auto& specs = result.value()->edge_specs();
  ASSERT_EQ(specs.size(), 2U);

  EXPECT_EQ(specs[0].config.barrier, BarrierType::Pipeline);
  EXPECT_EQ(specs[1].config.barrier, BarrierType::Full);
}

// =============================================================================
// §8.2 — Concatenating edge with flow control
// =============================================================================

TEST(GraphBuilderTest, EdgeConcatenatingWithFlowControl) {
  TestSource source{0};
  TestOp sink{1};

  GraphBuilder gb;
  gb.Edge(source, sink).Concatenating().FlowControl(10, 1024 * 1024);
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  auto& specs = result.value()->edge_specs();
  ASSERT_EQ(specs.size(), 1U);
  EXPECT_EQ(specs[0].config.type, cache::BatchHolderType::kConcatenating);
  EXPECT_EQ(specs[0].config.barrier, BarrierType::Full);
  EXPECT_EQ(specs[0].config.flow_control_batches_threshold, 10U);
  EXPECT_EQ(specs[0].config.flow_control_bytes_threshold, 1024U * 1024U);
}

// =============================================================================
// §8.3 — Two-input join (named ports + contract)
// =============================================================================

TEST(GraphBuilderTest, NamedPortsOnJoinSucceed) {
  TestSource table_a{0};
  TestSource table_b{1};
  TestJoinOp join{2};
  TestOp output{3};

  GraphBuilder gb;
  gb.Edge(table_a, join.probe());
  gb.Edge(table_b, join.build()).Spill(SpillPolicy::First);
  gb.Chain(join, output);
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  auto& specs = result.value()->edge_specs();
  ASSERT_EQ(specs.size(), 3U);

  // build-side edge: contract says Concatenating + Full; Spill set explicitly.
  EXPECT_EQ(specs[1].config.barrier, BarrierType::Full);
  EXPECT_EQ(specs[1].config.type, cache::BatchHolderType::kConcatenating);
  EXPECT_EQ(specs[1].config.spill_policy, SpillPolicy::First);
}

// =============================================================================
// §8.4 — Partitioned exchange (PartitionedUpTo)
// =============================================================================

TEST(GraphBuilderTest, PartitionedUpToStoresMaxPartitions) {
  TestSource scan{0};
  TestOp worker{1};

  GraphBuilder gb;
  gb.Edge(scan, worker).PartitionedUpTo(64).Named("exchange");
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  auto& specs = result.value()->edge_specs();
  ASSERT_EQ(specs.size(), 1U);
  EXPECT_EQ(specs[0].config.max_partitions, 64U);
}

// =============================================================================
// §8.5 — Diamond (one blocking producer, two consumers)
// =============================================================================

TEST(GraphBuilderTest, DiamondTopologyBuildsSuccessfully) {
  TestSource scan{0};
  TestOp agg{1};
  TestOp project_a{2};
  TestOp project_b{3};

  GraphBuilder gb;
  gb.Chain(scan, agg);
  gb.Edge(agg, project_a);
  gb.Edge(agg, project_b);
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  auto graph = std::move(*result);
  EXPECT_EQ(graph->GetNodeCount(), 4U);
}

// =============================================================================
// EdgeBuilder::ThenTo — continuation
// =============================================================================

TEST(GraphBuilderTest, ThenToCreatesChain) {
  TestSource a{0};
  TestOp b{1};
  CollectorOperator c{2};

  GraphBuilder gb;
  gb.Edge(a, b).ThenTo(c);
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  EXPECT_EQ(result.value()->GetNodeCount(), 3U);
  EXPECT_EQ(result.value()->edge_specs().size(), 2U);
}

// =============================================================================
// Apply() — shape helper
// =============================================================================

struct SimpleShape {
  Operator& src;
  Operator& dst;

  void WireInto(GraphBuilder& gb) { gb.Edge(src, dst); }
};

TEST(GraphBuilderTest, ApplyShapeWiresEdges) {
  TestSource scan{0};
  TestOp filter{1};

  GraphBuilder gb;
  gb.Apply(SimpleShape{scan, filter});
  auto result = std::move(gb).Build();
  ASSERT_TRUE(result.has_value()) << result.error().ToString();

  EXPECT_EQ(result.value()->edge_specs().size(), 1U);
}

// =============================================================================
// Build and Execute round-trip (§8.6 simplified)
// =============================================================================

TEST(GraphBuilderTest, BuildThenPrepareAndExecute) {
  TestSource scan{0};
  CollectorOperator collector{1};

  GraphBuilder gb;
  gb.Chain(scan, collector);
  auto build_result = std::move(gb).Build();
  ASSERT_TRUE(build_result.has_value()) << build_result.error().ToString();

  auto graph = std::move(*build_result);
  auto prepare_result = graph->Prepare(QueryStats{}, HardwareInfo{});
  ASSERT_TRUE(prepare_result.has_value()) << prepare_result.error().ToString();

  ASSERT_OK(prepare_result->Execute());
}

// =============================================================================
// Validation: (e) Partial() rejected
// =============================================================================

TEST(GraphBuilderTest, PartialBarrierRejectedAtBuild) {
  TestSource src{0};
  TestOp dst{1};

  GraphBuilder gb;
  gb.Edge(src, dst).Partial();
  auto result = std::move(gb).Build();

  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().ToString().find("Partial"), std::string::npos);
}

// =============================================================================
// Validation: (d) Duplicate edge names rejected
// =============================================================================

TEST(GraphBuilderTest, DuplicateEdgeNamesRejectedAtBuild) {
  TestSource a{0};
  TestOp b{1};
  TestOp c{2};

  GraphBuilder gb;
  gb.Edge(a, b).Named("my_edge");
  gb.Edge(a, c).Named("my_edge");
  auto result = std::move(gb).Build();

  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().ToString().find("duplicate"), std::string::npos);
}

// =============================================================================
// Validation: (a) Unknown port on operator with declared ports rejected
// =============================================================================

TEST(GraphBuilderTest, UnknownPortOnDeclaredPortsOperatorRejected) {
  TestSource table_a{0};
  TestJoinOp join{1};

  GraphBuilder gb;
  gb.Edge(table_a, join["invalid_port"]);  // typo: "invalid_port" not declared
  auto result = std::move(gb).Build();

  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().ToString().find("invalid_port"), std::string::npos);
}

// =============================================================================
// Validation: (b) Cycle detection
// =============================================================================

TEST(GraphBuilderTest, CycleRejectedAtBuild) {
  TestOp a{0};
  TestOp b{1};

  GraphBuilder gb;
  gb.Edge(a, b);
  gb.Edge(b, a);  // cycle: a→b→a
  auto result = std::move(gb).Build();

  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().ToString().find("cycle"), std::string::npos);
}

// =============================================================================
// Validation: (c) Barrier conflict with consumer contract
// =============================================================================

TEST(GraphBuilderTest, StreamingOnCollectorContradictsContractAndFails) {
  TestSource scan{0};
  CollectorOperator collector{1};

  GraphBuilder gb;
  gb.Edge(scan, collector).Streaming();  // contract says Full; author says Streaming
  auto result = std::move(gb).Build();

  EXPECT_FALSE(result.has_value());
  EXPECT_NE(result.error().ToString().find("barrier"), std::string::npos);
}

TEST(GraphBuilderTest, ConfigOverrideBypassesContractCheck) {
  // .Config() is the explicit escape hatch — no contract conflict check.
  TestSource scan{0};
  CollectorOperator collector{1};

  GraphBuilder gb;
  EdgeConfig custom_cfg;
  custom_cfg.barrier = BarrierType::Pipeline;  // would normally conflict
  gb.Edge(scan, collector).Config(custom_cfg);
  auto result = std::move(gb).Build();

  EXPECT_TRUE(result.has_value()) << (result.has_value() ? "" : result.error().ToString());
}

// =============================================================================
// CollectorOperator contract test
// =============================================================================

TEST(CollectorOperatorContractTest, DeclaredInputPortsIsEmpty) {
  CollectorOperator collector{0};
  EXPECT_TRUE(collector.DeclaredInputPorts().empty());
}

TEST(CollectorOperatorContractTest, InputEdgeContractReturnsFullBarrier) {
  CollectorOperator collector{0};
  EdgeConfig contract = collector.InputEdgeContract("");
  EXPECT_EQ(contract.barrier, BarrierType::Full);
}

// =============================================================================
// Operator base defaults
// =============================================================================

TEST(OperatorContractDefaults, DeclaredInputPortsDefaultEmpty) {
  TestOp op{0};
  EXPECT_TRUE(op.DeclaredInputPorts().empty());
}

TEST(OperatorContractDefaults, InputEdgeContractDefaultIsStreamingSimple) {
  TestOp op{0};
  EdgeConfig cfg = op.InputEdgeContract("any_port");
  EXPECT_EQ(cfg.barrier, BarrierType::Pipeline);
  EXPECT_EQ(cfg.type, cache::BatchHolderType::kSimple);
}

// =============================================================================
// SpillPolicy / EdgeConfig new fields
// =============================================================================

TEST(EdgeConfigTest, SpillPolicyFieldExistsWithDefaultEligible) {
  EdgeConfig cfg;
  EXPECT_EQ(cfg.spill_policy, SpillPolicy::Eligible);
}

TEST(EdgeConfigTest, MaxPartitionsFieldExistsWithDefaultZero) {
  EdgeConfig cfg;
  EXPECT_EQ(cfg.max_partitions, 0U);
}

}  // namespace
