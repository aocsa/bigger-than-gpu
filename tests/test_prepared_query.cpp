// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>

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

TEST(PreparedQueryTest, AddEdgeDoesNotMaterializeHolders) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Chain(source, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  EXPECT_EQ(graph->edge_specs().size(), 1U);
  EXPECT_FALSE(source.output().has_port(std::to_string(source.id())));
  EXPECT_FALSE(sink.input().has_port(std::to_string(sink.id())));
}

TEST(PreparedQueryTest, MaterializeRegistersHoldersOnPorts) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Chain(source, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();

  EXPECT_TRUE(source.output().has_port(std::to_string(source.id())));
  EXPECT_TRUE(sink.input().has_port(std::to_string(sink.id())));
  EXPECT_NE(source.output().get(std::to_string(source.id())), nullptr);
  EXPECT_EQ(source.output().get(std::to_string(source.id())),
            sink.input().get(std::to_string(sink.id())));
}

TEST(PreparedQueryTest, PrepareBuildsPipelinesAndIsIndependentAcrossCalls) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Chain(source, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto first = graph->Prepare(QueryStats{}, HardwareInfo{});
  ASSERT_TRUE(first.has_value()) << first.error().ToString();
  EXPECT_EQ(first->holder_count(), 1U);
  EXPECT_EQ(first->pipeline_count(), 1U);

  // Re-Prepare yields another independent PreparedQuery (fresh holders).
  auto second = graph->Prepare(QueryStats{.estimated_rows = 1000}, HardwareInfo{.cores = 4});
  ASSERT_TRUE(second.has_value()) << second.error().ToString();
  EXPECT_EQ(second->holder_count(), 1U);
  EXPECT_EQ(second->pipeline_count(), 1U);

  ASSERT_OK(first->Execute());
  ASSERT_OK(second->Execute());
}

TEST(PreparedQueryTest, ExecuteIsPrepareSugar) {
  MockSource source{0};
  MockSink sink{1};

  GraphBuilder gb;
  gb.Chain(source, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  ASSERT_OK(graph->Execute());
}

}  // namespace
