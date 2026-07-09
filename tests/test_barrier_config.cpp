// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_barrier_config.cpp
/// @brief Tests for barrier semantics on edges (BarrierType, EdgeConfig)
///        and the blocking-operator classification.
///
/// Stage 1 of docs/dev/pipeline-execution-plan.md: the vocabulary that lets
/// the meta-pipeline builder split the operator DAG at blocking operators.

#include <gtest/gtest.h>

#include "quest/taskflow/kpair.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/operator_type.hpp"

namespace quest::taskflow {
namespace {

// =============================================================================
// BarrierType on EdgeConfig
// =============================================================================

TEST(BarrierConfigTest, DefaultEdgeConfigIsPipelineBarrier) {
  const EdgeConfig config{};
  EXPECT_EQ(config.barrier, BarrierType::Pipeline);
  EXPECT_EQ(config.type, cache::BatchHolderType::kSimple);
}

TEST(BarrierConfigTest, SimpleFactoryIsPipelineBarrier) {
  const auto config = EdgeConfig::Simple();
  EXPECT_EQ(config.barrier, BarrierType::Pipeline);
}

TEST(BarrierConfigTest, ConcatenatingFactoryImpliesFullBarrier) {
  const auto config = EdgeConfig::Concatenating();
  EXPECT_EQ(config.type, cache::BatchHolderType::kConcatenating);
  EXPECT_EQ(config.barrier, BarrierType::Full);
}

TEST(BarrierConfigTest, PartitionedFactoryIsPipelineBarrier) {
  const auto config = EdgeConfig::Partitioned(4);
  EXPECT_EQ(config.barrier, BarrierType::Pipeline);
  EXPECT_EQ(config.num_partitions, 4U);
}

TEST(BarrierConfigTest, WithBarrierChainsLikeWithFlowControl) {
  auto config = EdgeConfig::Simple().with_barrier(BarrierType::Full);
  EXPECT_EQ(config.barrier, BarrierType::Full);

  // Chaining order must not matter.
  auto chained =
      EdgeConfig::Simple().with_barrier(BarrierType::Full).with_flow_control(8);
  EXPECT_EQ(chained.barrier, BarrierType::Full);
  EXPECT_EQ(chained.flow_control_batches_threshold, 8U);
}

TEST(BarrierConfigTest, BarrierToString) {
  EXPECT_EQ(to_string(BarrierType::Pipeline), "Pipeline");
  EXPECT_EQ(to_string(BarrierType::Full), "Full");
}

// =============================================================================
// DSL round-trip: the KPair must carry the barrier to the graph
// =============================================================================

class NoOpOperator : public Operator {
 public:
  NoOpOperator(std::uint32_t id, OperatorType type)
      : Operator(id, "noop", type) {}

  [[nodiscard]] ExecutionStatus run() override {
    return ExecutionStatus::Proceed;
  }
  [[nodiscard]] bool can_you_throttle_my_input() const override {
    return true;
  }
};

TEST(BarrierConfigTest, LinkCarriesFullBarrierThroughKPair) {
  NoOpOperator source{0, OperatorType::TableScan};
  NoOpOperator sink{1, OperatorType::ComputeAggregate};

  const KPair edge = link(source, sink, EdgeConfig::Concatenating());
  EXPECT_EQ(edge.config().barrier, BarrierType::Full);
  EXPECT_EQ(edge.config().type, cache::BatchHolderType::kConcatenating);
}

TEST(BarrierConfigTest, ShiftOperatorDefaultsToPipelineBarrier) {
  NoOpOperator source{0, OperatorType::TableScan};
  NoOpOperator sink{1, OperatorType::Filter};

  const KPair edge = source >> sink;
  EXPECT_EQ(edge.config().barrier, BarrierType::Pipeline);
}

TEST(BarrierConfigTest, LinkToNamedPortCarriesBarrier) {
  NoOpOperator source{0, OperatorType::TableScan};
  NoOpOperator join{1, OperatorType::PartwiseJoin};

  const KPair edge = link(source, join["build"],
                          EdgeConfig::Simple().with_barrier(BarrierType::Full));
  EXPECT_EQ(edge.config().barrier, BarrierType::Full);
  EXPECT_EQ(edge.destination_port(), "build");
}

// =============================================================================
// Blocking-operator classification
// =============================================================================

TEST(BlockingOperatorTest, BlockingTypes) {
  EXPECT_TRUE(is_blocking_operator(OperatorType::ComputeAggregate));
  EXPECT_TRUE(is_blocking_operator(OperatorType::DistributeAggregate));
  EXPECT_TRUE(is_blocking_operator(OperatorType::MergeAggregate));
  EXPECT_TRUE(is_blocking_operator(OperatorType::SortAndSample));
  EXPECT_TRUE(is_blocking_operator(OperatorType::Partition));
  EXPECT_TRUE(is_blocking_operator(OperatorType::PartitionSingleNode));
  EXPECT_TRUE(is_blocking_operator(OperatorType::MergeStream));
  EXPECT_TRUE(is_blocking_operator(OperatorType::PartwiseJoin));
  EXPECT_TRUE(is_blocking_operator(OperatorType::JoinPartition));
  EXPECT_TRUE(is_blocking_operator(OperatorType::Union));
}

TEST(BlockingOperatorTest, StreamingAndSourceTypesAreNotBlocking) {
  EXPECT_FALSE(is_blocking_operator(OperatorType::TableScan));
  EXPECT_FALSE(is_blocking_operator(OperatorType::BindableTableScan));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Generate));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Filter));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Project));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Limit));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Output));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Print));
  EXPECT_FALSE(is_blocking_operator(OperatorType::Unknown));
}

TEST(BlockingOperatorTest, ClassificationIsConstexpr) {
  static_assert(is_blocking_operator(OperatorType::ComputeAggregate));
  static_assert(!is_blocking_operator(OperatorType::Filter));
}

TEST(BlockingOperatorTest, OperatorIsBlockingConsultsType) {
  NoOpOperator aggregate{0, OperatorType::ComputeAggregate};
  NoOpOperator filter{1, OperatorType::Filter};
  EXPECT_TRUE(aggregate.IsBlocking());
  EXPECT_FALSE(filter.IsBlocking());
}

TEST(BlockingOperatorTest, IsBlockingIsOverridable) {
  // Mirrors Sirius: an operator may deviate from its type's default
  // (e.g. a streaming-capable aggregate).
  class StreamingAggregate : public NoOpOperator {
   public:
    using NoOpOperator::NoOpOperator;
    [[nodiscard]] bool IsBlocking() const noexcept override { return false; }
  };

  StreamingAggregate op{0, OperatorType::ComputeAggregate};
  EXPECT_FALSE(op.IsBlocking());
}

}  // namespace
}  // namespace quest::taskflow
