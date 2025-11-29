/// @file test_graph.cpp
/// @brief Comprehensive tests for the execution graph and related components.

#include <gtest/gtest.h>

#include <quest/quest.hpp>

using namespace quest;
using namespace quest::taskflow;
using namespace quest::cache;

// =============================================================================
// Test Helper Operators
// =============================================================================

/// @brief Simple operator that counts how many times it was invoked.
class CountingOperator : public Operator {
 public:
  CountingOperator(std::uint32_t operator_id, std::size_t& invocation_counter)
      : Operator(operator_id, "Counter", OperatorType::Project),
        invocation_counter_(invocation_counter) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  [[nodiscard]] ExecutionStatus run() override {
    ++invocation_counter_;
    return ExecutionStatus::Proceed;
  }

 private:
  std::size_t& invocation_counter_;
};

/// @brief Source operator that produces a configurable number of data batches.
class SourceOperator : public Operator {
 public:
  SourceOperator(std::uint32_t operator_id, std::size_t batch_count)
      : Operator(operator_id, "Source", OperatorType::TableScan),
        batch_count_(batch_count) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }

  [[nodiscard]] ExecutionStatus run() override {
    for (std::size_t batch_index = 0; batch_index < batch_count_;
         ++batch_index) {
      auto batch_data = std::make_shared<CPUCacheData>(
          std::format("batch_{}", batch_index),
          std::vector<std::byte>(100),
          10,
          2);

      (void)add_to_output(std::move(batch_data));
    }
    return ExecutionStatus::Proceed;
  }

 private:
  std::size_t batch_count_;
};

/// @brief Sink operator that counts received batches.
class SinkOperator : public Operator {
 public:
  SinkOperator(std::uint32_t operator_id, std::size_t& received_batch_count)
      : Operator(operator_id, "Sink", OperatorType::Output),
        received_batch_count_(received_batch_count) {}

  [[nodiscard]] bool can_you_throttle_my_input() const override { return false; }

  [[nodiscard]] ExecutionStatus run() override {
    auto batch_holder = input_cache();

    while (batch_holder) {
      auto pull_result = batch_holder->pull();
      if (!pull_result.has_value()) {
        break;
      }
      if (pull_result.value()) {
        ++received_batch_count_;
      }
    }
    return ExecutionStatus::Proceed;
  }

 private:
  std::size_t& received_batch_count_;
};

/// @brief Passthrough operator that forwards data unchanged.
class PassthroughOperator : public SimpleOperator {
 public:
  explicit PassthroughOperator(std::uint32_t operator_id)
      : SimpleOperator(operator_id, "Passthrough", OperatorType::Project) {}

 protected:
  [[nodiscard]] Result<std::shared_ptr<CacheData>> process_batch(
      std::shared_ptr<CacheData> input) override {
    return input;
  }
};

/// @brief Multi-input operator simulating a join with two inputs.
class JoinOperator : public Operator {
 public:
  JoinOperator(std::uint32_t operator_id,
               std::size_t& left_received_count,
               std::size_t& right_received_count)
      : Operator(operator_id, "Join", OperatorType::PartwiseJoin),
        left_received_count_(left_received_count),
        right_received_count_(right_received_count) {
    input().register_port("left_input");
    input().register_port("right_input");
  }

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  [[nodiscard]] ExecutionStatus run() override {
    auto& left_cache = input()["left_input"];
    while (left_cache) {
      auto pull_result = left_cache->pull();
      if (!pull_result.has_value()) {
        break;
      }
      ++left_received_count_;
    }

    auto& right_cache = input()["right_input"];
    while (right_cache) {
      auto pull_result = right_cache->pull();
      if (!pull_result.has_value()) {
        break;
      }
      ++right_received_count_;
    }

    return ExecutionStatus::Proceed;
  }

 private:
  std::size_t& left_received_count_;
  std::size_t& right_received_count_;
};

// =============================================================================
// Graph Construction Tests
// =============================================================================

TEST(GraphConstructionTest, EmptyGraph) {
  auto graph = std::make_shared<Graph>();

  EXPECT_EQ(graph->GetNodeCount(), 0);
  EXPECT_EQ(graph->GetNode(0), nullptr);
  EXPECT_EQ(graph->GetNode(999), nullptr);
}

TEST(GraphConstructionTest, SingleNode) {
  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator single_operator(0, counter);

  auto node_id = graph->AddNode(&single_operator);

  EXPECT_EQ(node_id, 0);
  EXPECT_EQ(graph->GetNodeCount(), 1);
  EXPECT_EQ(graph->GetNode(0), &single_operator);
  EXPECT_EQ(&graph->GetLastOperator(), &single_operator);
}

TEST(GraphConstructionTest, SimpleEdge) {
  auto graph = std::make_shared<Graph>();

  std::size_t counter_a = 0;
  std::size_t counter_b = 0;
  CountingOperator operator_a(0, counter_a);
  CountingOperator operator_b(1, counter_b);

  *graph += operator_a >> operator_b;

  EXPECT_EQ(graph->GetNodeCount(), 2);
  EXPECT_EQ(graph->GetNode(0), &operator_a);
  EXPECT_EQ(graph->GetNode(1), &operator_b);

  auto outgoing = graph->GetOutgoingEdges(&operator_a);
  ASSERT_EQ(outgoing.size(), 1);
  EXPECT_EQ(outgoing.begin()->target_operator_id, 1);

  auto incoming = graph->GetIncomingEdges(&operator_b);
  ASSERT_EQ(incoming.size(), 1);
  EXPECT_EQ(incoming.begin()->source_operator_id, 0);
}

TEST(GraphConstructionTest, LinearChain) {
  auto graph = std::make_shared<Graph>();

  std::size_t counters[4] = {0, 0, 0, 0};
  CountingOperator operators[4] = {
      {0, counters[0]}, {1, counters[1]}, {2, counters[2]}, {3, counters[3]}};

  *graph += operators[0] >> operators[1];
  *graph += operators[1] >> operators[2];
  *graph += operators[2] >> operators[3];

  EXPECT_EQ(graph->GetNodeCount(), 4);

  for (int node_index = 0; node_index < 3; ++node_index) {
    auto outgoing = graph->GetOutgoingEdges(&operators[node_index]);
    ASSERT_EQ(outgoing.size(), 1);
    EXPECT_EQ(outgoing.begin()->target_operator_id, node_index + 1);
  }
}

TEST(GraphConstructionTest, DiamondTopology) {
  auto graph = std::make_shared<Graph>();

  std::size_t counters[4] = {0, 0, 0, 0};
  CountingOperator operator_a(0, counters[0]);
  CountingOperator operator_b(1, counters[1]);
  CountingOperator operator_c(2, counters[2]);
  CountingOperator operator_d(3, counters[3]);

  *graph += operator_a >> operator_b;
  *graph += operator_a >> operator_c;
  *graph += operator_b >> operator_d;
  *graph += operator_c >> operator_d;

  EXPECT_EQ(graph->GetNodeCount(), 4);

  auto a_outgoing = graph->GetOutgoingEdges(&operator_a);
  EXPECT_EQ(a_outgoing.size(), 2);

  auto d_incoming = graph->GetIncomingEdges(&operator_d);
  EXPECT_EQ(d_incoming.size(), 2);
}

// =============================================================================
// Graph Execution Tests
// =============================================================================

TEST(GraphExecutionTest, SimpleExecution) {
  auto graph = std::make_shared<Graph>();

  std::size_t received_count = 0;
  SourceOperator source(0, 5);
  SinkOperator sink(1, received_count);

  *graph += source >> sink;

  auto status = graph->Execute();
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(received_count, 5);
}

TEST(GraphExecutionTest, PassthroughExecution) {
  auto graph = std::make_shared<Graph>();

  std::size_t received_count = 0;
  SourceOperator source(0, 10);
  PassthroughOperator passthrough(1);
  SinkOperator sink(2, received_count);

  *graph += source >> passthrough;
  *graph += passthrough >> sink;

  auto status = graph->Execute();
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(received_count, 10);
}

TEST(GraphExecutionTest, NamedPortsExecution) {
  auto graph = std::make_shared<Graph>();

  std::size_t left_count = 0;
  std::size_t right_count = 0;

  SourceOperator source_a(0, 3);
  SourceOperator source_b(1, 4);
  JoinOperator join_op(2, left_count, right_count);

  *graph += source_a >> join_op["left_input"];
  *graph += source_b >> join_op["right_input"];

  auto status = graph->Execute();
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(left_count, 3);
  EXPECT_EQ(right_count, 4);
}

TEST(GraphExecutionTest, ConcatenatingBatchHolder) {
  auto graph = std::make_shared<Graph>();

  std::size_t received_count = 0;
  SourceOperator source(0, 5);
  SinkOperator sink(1, received_count);

  *graph +=
      link(source, sink, EdgeConfig::Concatenating().with_flow_control(10, 1024 * 1024));

  auto status = graph->Execute();
  ASSERT_TRUE(status.ok());
  EXPECT_EQ(received_count, 1);
}

// =============================================================================
// Edge Structure Tests
// =============================================================================

TEST(EdgeStructureTest, EdgeComparison) {
  Edge edge_a{.source_operator_id = 0,
              .target_operator_id = 1,
              .source_port_name = "out",
              .target_port_name = "in"};

  Edge edge_b{.source_operator_id = 0,
              .target_operator_id = 1,
              .source_port_name = "out",
              .target_port_name = "in"};

  Edge edge_c{.source_operator_id = 0,
              .target_operator_id = 2,
              .source_port_name = "out",
              .target_port_name = "in"};

  EXPECT_EQ(edge_a, edge_b);
  EXPECT_NE(edge_a, edge_c);
  EXPECT_LT(edge_a, edge_c);
}

TEST(EdgeStructureTest, EdgeSetUniqueness) {
  std::set<Edge> edge_set;

  Edge edge_1{.source_operator_id = 0,
              .target_operator_id = 1,
              .source_port_name = "a",
              .target_port_name = "b"};

  Edge edge_2{.source_operator_id = 0,
              .target_operator_id = 1,
              .source_port_name = "c",
              .target_port_name = "d"};

  edge_set.insert(edge_1);
  edge_set.insert(edge_2);

  EXPECT_EQ(edge_set.size(), 1);
}

// =============================================================================
// Graph Query Tests
// =============================================================================

TEST(GraphQueryTest, GetLastOperator) {
  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator op1(0, counter);
  CountingOperator op2(1, counter);
  CountingOperator op3(2, counter);

  *graph += op1 >> op2;
  *graph += op2 >> op3;

  EXPECT_EQ(&graph->GetLastOperator(), &op3);
}

TEST(GraphQueryTest, GetLastOperatorThrowsWhenEmpty) {
  auto graph = std::make_shared<Graph>();

  EXPECT_THROW(graph->GetLastOperator(), std::runtime_error);
}

TEST(GraphQueryTest, GetNonexistentNode) {
  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator op(0, counter);
  graph->AddNode(&op);

  EXPECT_EQ(graph->GetNode(999), nullptr);
}

// =============================================================================
// KPair and EdgeConfig Tests
// =============================================================================

TEST(EdgeConfigTest, Simple) {
  auto config = EdgeConfig::Simple();

  EXPECT_EQ(config.type, BatchHolderType::Simple);
  EXPECT_EQ(config.num_partitions, 1);
}

TEST(EdgeConfigTest, Concatenating) {
  auto config = EdgeConfig::Concatenating();

  EXPECT_EQ(config.type, BatchHolderType::Concatenating);
  EXPECT_EQ(config.num_partitions, 1);
}

TEST(EdgeConfigTest, Partitioned) {
  auto config = EdgeConfig::Partitioned(4);

  EXPECT_EQ(config.type, BatchHolderType::Simple);
  EXPECT_EQ(config.num_partitions, 4);
}

TEST(EdgeConfigTest, FlowControl) {
  auto config = EdgeConfig::Simple().with_flow_control(10, 1024);

  EXPECT_EQ(config.flow_control_batches_threshold, 10);
  EXPECT_EQ(config.flow_control_bytes_threshold, 1024);
}

TEST(KPairTest, Accessors) {
  std::size_t counter = 0;
  CountingOperator source(0, counter);
  CountingOperator dest(1, counter);

  KPair pair(source, dest);

  EXPECT_EQ(pair.source(), &source);
  EXPECT_EQ(pair.destination(), &dest);
  EXPECT_FALSE(pair.has_custom_source());
  EXPECT_FALSE(pair.has_custom_destination());
}

TEST(KPairTest, WithNamedPorts) {
  std::size_t counter = 0;
  CountingOperator source(0, counter);
  CountingOperator dest(1, counter);

  KPair pair(source, dest["input_port"]);

  EXPECT_EQ(pair.source(), &source);
  EXPECT_EQ(pair.destination(), &dest);
  EXPECT_FALSE(pair.has_custom_source());
  EXPECT_TRUE(pair.has_custom_destination());
  EXPECT_EQ(pair.destination_port(), "input_port");
}

// =============================================================================
// Operator Parent Relationship Tests
// =============================================================================

TEST(OperatorRelationshipTest, ParentRelationship) {
  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator op1(0, counter);
  CountingOperator op2(1, counter);

  EXPECT_FALSE(op1.has_parent());
  EXPECT_FALSE(op2.has_parent());

  *graph += op1 >> op2;

  EXPECT_FALSE(op1.has_parent());
  EXPECT_TRUE(op2.has_parent());
  EXPECT_EQ(op2.parent_id(), 0);
}

// =============================================================================
// Memory Manager Tests
// =============================================================================

TEST(MemoryManagerTest, GraphMemoryManager) {
  auto memory_manager = make_default_memory_manager();
  auto graph = std::make_shared<Graph>(memory_manager);

  EXPECT_EQ(graph->GetMemoryManager(), memory_manager);

  auto new_manager = make_default_memory_manager();
  graph->SetMemoryManager(new_manager);
  EXPECT_EQ(graph->GetMemoryManager(), new_manager);
}

// =============================================================================
// Operator Execution State Tests
// =============================================================================

TEST(OperatorStateTest, ExecutionState) {
  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  EXPECT_FALSE(test_operator.is_done());

  test_operator.mark_done();
  EXPECT_TRUE(test_operator.is_done());
}

TEST(OperatorStateTest, LimitOptimization) {
  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  EXPECT_FALSE(test_operator.has_limit());
  EXPECT_EQ(test_operator.limit_rows(), -1);

  test_operator.set_limit(100);
  EXPECT_TRUE(test_operator.has_limit());
  EXPECT_EQ(test_operator.limit_rows(), 100);
}

// =============================================================================
// Port Registration Tests
// =============================================================================

TEST(PortTest, Registration) {
  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  auto& input_port = test_operator.input();
  input_port.register_port("port_a");
  input_port.register_port("port_b");

  EXPECT_TRUE(input_port.has_port("port_a"));
  EXPECT_TRUE(input_port.has_port("port_b"));
  EXPECT_FALSE(input_port.has_port("port_c"));
  EXPECT_EQ(input_port.count(), 2);
}

TEST(PortTest, AddPortsHelper) {
  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  add_ports(test_operator.input(), "left", "right", "probe");

  EXPECT_TRUE(test_operator.input().has_port("left"));
  EXPECT_TRUE(test_operator.input().has_port("right"));
  EXPECT_TRUE(test_operator.input().has_port("probe"));
  EXPECT_EQ(test_operator.input().count(), 3);
}
