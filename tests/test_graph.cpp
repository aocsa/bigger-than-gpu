/// @file test_graph.cpp
/// @brief Comprehensive tests for the execution graph and related components.

#include <quest/quest.hpp>

#include <cassert>
#include <print>

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
    // Produce the configured number of batches.
    for (std::size_t batch_index = 0; batch_index < batch_count_;
         ++batch_index) {
      auto batch_data = std::make_shared<CPUCacheData>(
          std::format("batch_{}", batch_index),
          std::vector<std::byte>(100),  // 100 bytes per batch
          10,                            // 10 rows per batch
          2);                            // 2 columns

      add_to_output(std::move(batch_data));
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

    // Pull all available batches.
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
  PassthroughOperator(std::uint32_t operator_id)
      : SimpleOperator(operator_id, "Passthrough", OperatorType::Project) {}

 protected:
  [[nodiscard]] Result<std::shared_ptr<CacheData>> process_batch(
      std::shared_ptr<CacheData> input) override {
    // Simply forward the input unchanged.
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
    // Register named input ports.
    input().register_port("left_input");
    input().register_port("right_input");
  }

  [[nodiscard]] bool can_you_throttle_my_input() const override { return true; }

  [[nodiscard]] ExecutionStatus run() override {
    // Process left input.
    auto& left_cache = input()["left_input"];
    while (left_cache) {
      auto pull_result = left_cache->pull();
      if (!pull_result.has_value()) {
        break;
      }
      ++left_received_count_;
    }

    // Process right input.
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

void test_empty_graph() {
  std::print("test_empty_graph... ");

  auto graph = std::make_shared<Graph>();

  // Verify empty state.
  assert(graph->GetNodeCount() == 0);
  assert(graph->GetNode(0) == nullptr);
  assert(graph->GetNode(999) == nullptr);

  std::print("PASSED\n");
}

void test_single_node() {
  std::print("test_single_node... ");

  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator single_operator(0, counter);

  // Add single node explicitly.
  auto node_id = graph->AddNode(&single_operator);

  assert(node_id == 0);
  assert(graph->GetNodeCount() == 1);
  assert(graph->GetNode(0) == &single_operator);
  assert(&graph->GetLastOperator() == &single_operator);

  std::print("PASSED\n");
}

void test_simple_edge() {
  std::print("test_simple_edge... ");

  auto graph = std::make_shared<Graph>();

  std::size_t counter_a = 0;
  std::size_t counter_b = 0;
  CountingOperator operator_a(0, counter_a);
  CountingOperator operator_b(1, counter_b);

  // Add edge using DSL.
  *graph += operator_a >> operator_b;

  // Verify graph structure.
  assert(graph->GetNodeCount() == 2);
  assert(graph->GetNode(0) == &operator_a);
  assert(graph->GetNode(1) == &operator_b);

  // Verify edge connectivity.
  auto outgoing = graph->GetOutgoingEdges(&operator_a);
  assert(outgoing.size() == 1);
  assert(outgoing.begin()->target_operator_id == 1);

  auto incoming = graph->GetIncomingEdges(&operator_b);
  assert(incoming.size() == 1);
  assert(incoming.begin()->source_operator_id == 0);

  std::print("PASSED\n");
}

void test_linear_chain() {
  std::print("test_linear_chain... ");

  auto graph = std::make_shared<Graph>();

  std::size_t counters[4] = {0, 0, 0, 0};
  CountingOperator operators[4] = {
      {0, counters[0]}, {1, counters[1]}, {2, counters[2]}, {3, counters[3]}};

  // Build linear chain.
  *graph += operators[0] >> operators[1];
  *graph += operators[1] >> operators[2];
  *graph += operators[2] >> operators[3];

  // Verify graph structure.
  assert(graph->GetNodeCount() == 4);

  // Verify connectivity.
  for (int node_index = 0; node_index < 3; ++node_index) {
    auto outgoing = graph->GetOutgoingEdges(&operators[node_index]);
    assert(outgoing.size() == 1);
    assert(outgoing.begin()->target_operator_id == node_index + 1);
  }

  std::print("PASSED\n");
}

void test_diamond_topology() {
  std::print("test_diamond_topology... ");

  // Diamond: A -> B, A -> C, B -> D, C -> D
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

  assert(graph->GetNodeCount() == 4);

  // Node A should have 2 outgoing edges.
  auto a_outgoing = graph->GetOutgoingEdges(&operator_a);
  assert(a_outgoing.size() == 2);

  // Node D should have 2 incoming edges.
  auto d_incoming = graph->GetIncomingEdges(&operator_d);
  assert(d_incoming.size() == 2);

  std::print("PASSED\n");
}

// =============================================================================
// Graph Execution Tests
// =============================================================================

void test_simple_execution() {
  std::print("test_simple_execution... ");

  auto graph = std::make_shared<Graph>();

  std::size_t received_count = 0;
  SourceOperator source(0, 5);  // Produces 5 batches
  SinkOperator sink(1, received_count);

  *graph += source >> sink;

  auto status = graph->Execute();
  assert(status.ok());
  assert(received_count == 5);

  std::print("PASSED\n");
}

void test_passthrough_execution() {
  std::print("test_passthrough_execution... ");

  auto graph = std::make_shared<Graph>();

  std::size_t received_count = 0;
  SourceOperator source(0, 10);
  PassthroughOperator passthrough(1);
  SinkOperator sink(2, received_count);

  *graph += source >> passthrough;
  *graph += passthrough >> sink;

  auto status = graph->Execute();
  assert(status.ok());
  assert(received_count == 10);

  std::print("PASSED\n");
}

void test_named_ports_execution() {
  std::print("test_named_ports_execution... ");

  auto graph = std::make_shared<Graph>();

  std::size_t left_count = 0;
  std::size_t right_count = 0;

  SourceOperator source_a(0, 3);
  SourceOperator source_b(1, 4);
  JoinOperator join_op(2, left_count, right_count);

  // Connect using named port DSL.
  *graph += source_a >> join_op["left_input"];
  *graph += source_b >> join_op["right_input"];

  auto status = graph->Execute();
  assert(status.ok());
  assert(left_count == 3);
  assert(right_count == 4);

  std::print("PASSED\n");
}

void test_concatenating_batch_holder() {
  std::print("test_concatenating_batch_holder... ");

  auto graph = std::make_shared<Graph>();

  std::size_t received_count = 0;
  SourceOperator source(0, 5);
  SinkOperator sink(1, received_count);

  // Use concatenating batch holder (merges all batches into one).
  *graph +=
      link(source, sink, EdgeConfig::Concatenating().with_flow_control(10, 1024 * 1024));

  auto status = graph->Execute();
  assert(status.ok());
  // Concatenating holder returns all batches merged as one.
  assert(received_count == 1);

  std::print("PASSED\n");
}

// =============================================================================
// Edge Structure Tests
// =============================================================================

void test_edge_comparison() {
  std::print("test_edge_comparison... ");

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

  // Test equality.
  assert(edge_a == edge_b);
  assert(!(edge_a == edge_c));

  // Test ordering (by target, then source).
  assert(edge_a < edge_c);  // target 1 < target 2

  std::print("PASSED\n");
}

void test_edge_set_uniqueness() {
  std::print("test_edge_set_uniqueness... ");

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
  edge_set.insert(edge_2);  // Same source/target, different ports

  // Edges with same source/target should be considered equal.
  assert(edge_set.size() == 1);

  std::print("PASSED\n");
}

// =============================================================================
// Graph Query Tests
// =============================================================================

void test_get_last_operator() {
  std::print("test_get_last_operator... ");

  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator op1(0, counter);
  CountingOperator op2(1, counter);
  CountingOperator op3(2, counter);

  *graph += op1 >> op2;
  *graph += op2 >> op3;

  // Last operator should be op3.
  assert(&graph->GetLastOperator() == &op3);

  std::print("PASSED\n");
}

void test_get_last_operator_throws_when_empty() {
  std::print("test_get_last_operator_throws_when_empty... ");

  auto graph = std::make_shared<Graph>();

  bool exception_thrown = false;
  try {
    [[maybe_unused]] auto& last = graph->GetLastOperator();
  } catch (const std::runtime_error&) {
    exception_thrown = true;
  }

  assert(exception_thrown);

  std::print("PASSED\n");
}

void test_get_nonexistent_node() {
  std::print("test_get_nonexistent_node... ");

  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator op(0, counter);
  graph->AddNode(&op);

  // Query for non-existent node.
  assert(graph->GetNode(999) == nullptr);

  std::print("PASSED\n");
}

// =============================================================================
// KPair and EdgeConfig Tests
// =============================================================================

void test_edge_config_simple() {
  std::print("test_edge_config_simple... ");

  auto config = EdgeConfig::Simple();

  assert(config.type == BatchHolderType::Simple);
  assert(config.num_partitions == 1);

  std::print("PASSED\n");
}

void test_edge_config_concatenating() {
  std::print("test_edge_config_concatenating... ");

  auto config = EdgeConfig::Concatenating();

  assert(config.type == BatchHolderType::Concatenating);
  assert(config.num_partitions == 1);

  std::print("PASSED\n");
}

void test_edge_config_partitioned() {
  std::print("test_edge_config_partitioned... ");

  auto config = EdgeConfig::Partitioned(4);

  assert(config.type == BatchHolderType::Simple);
  assert(config.num_partitions == 4);

  std::print("PASSED\n");
}

void test_edge_config_flow_control() {
  std::print("test_edge_config_flow_control... ");

  auto config = EdgeConfig::Simple().with_flow_control(10, 1024);

  assert(config.flow_control_batches_threshold == 10);
  assert(config.flow_control_bytes_threshold == 1024);

  std::print("PASSED\n");
}

void test_kpair_accessors() {
  std::print("test_kpair_accessors... ");

  std::size_t counter = 0;
  CountingOperator source(0, counter);
  CountingOperator dest(1, counter);

  KPair pair(source, dest);

  assert(pair.source() == &source);
  assert(pair.destination() == &dest);
  assert(!pair.has_custom_source());
  assert(!pair.has_custom_destination());

  std::print("PASSED\n");
}

void test_kpair_with_named_ports() {
  std::print("test_kpair_with_named_ports... ");

  std::size_t counter = 0;
  CountingOperator source(0, counter);
  CountingOperator dest(1, counter);

  // Create pair with named destination port.
  KPair pair(source, dest["input_port"]);

  assert(pair.source() == &source);
  assert(pair.destination() == &dest);
  assert(!pair.has_custom_source());
  assert(pair.has_custom_destination());
  assert(pair.destination_port() == "input_port");

  std::print("PASSED\n");
}

// =============================================================================
// Operator Parent Relationship Tests
// =============================================================================

void test_operator_parent_relationship() {
  std::print("test_operator_parent_relationship... ");

  auto graph = std::make_shared<Graph>();

  std::size_t counter = 0;
  CountingOperator op1(0, counter);
  CountingOperator op2(1, counter);

  // Before adding to graph, operators have no parent.
  assert(!op1.has_parent());
  assert(!op2.has_parent());

  *graph += op1 >> op2;

  // After adding edge, op2 should have op1 as parent.
  assert(!op1.has_parent());  // Source remains root
  assert(op2.has_parent());
  assert(op2.parent_id() == 0);

  std::print("PASSED\n");
}

// =============================================================================
// Memory Manager Tests
// =============================================================================

void test_graph_memory_manager() {
  std::print("test_graph_memory_manager... ");

  auto memory_manager = make_default_memory_manager();
  auto graph = std::make_shared<Graph>(memory_manager);

  assert(graph->GetMemoryManager() == memory_manager);

  // Test setting new manager.
  auto new_manager = make_default_memory_manager();
  graph->SetMemoryManager(new_manager);
  assert(graph->GetMemoryManager() == new_manager);

  std::print("PASSED\n");
}

// =============================================================================
// Operator Execution State Tests
// =============================================================================

void test_operator_execution_state() {
  std::print("test_operator_execution_state... ");

  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  // Initially not done.
  assert(!test_operator.is_done());

  // Mark as done.
  test_operator.mark_done();
  assert(test_operator.is_done());

  std::print("PASSED\n");
}

void test_operator_limit_optimization() {
  std::print("test_operator_limit_optimization... ");

  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  // Initially no limit.
  assert(!test_operator.has_limit());
  assert(test_operator.limit_rows() == -1);

  // Apply limit.
  test_operator.set_limit(100);
  assert(test_operator.has_limit());
  assert(test_operator.limit_rows() == 100);

  std::print("PASSED\n");
}

// =============================================================================
// Port Registration Tests
// =============================================================================

void test_port_registration() {
  std::print("test_port_registration... ");

  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  // Register ports on input.
  auto& input_port = test_operator.input();
  input_port.register_port("port_a");
  input_port.register_port("port_b");

  assert(input_port.has_port("port_a"));
  assert(input_port.has_port("port_b"));
  assert(!input_port.has_port("port_c"));
  assert(input_port.count() == 2);

  std::print("PASSED\n");
}

void test_add_ports_helper() {
  std::print("test_add_ports_helper... ");

  std::size_t counter = 0;
  CountingOperator test_operator(0, counter);

  // Use variadic helper.
  add_ports(test_operator.input(), "left", "right", "probe");

  assert(test_operator.input().has_port("left"));
  assert(test_operator.input().has_port("right"));
  assert(test_operator.input().has_port("probe"));
  assert(test_operator.input().count() == 3);

  std::print("PASSED\n");
}

// =============================================================================
// Main Test Runner
// =============================================================================

int main() {
  std::print("=== Graph Tests ===\n\n");

  // Graph construction tests.
  test_empty_graph();
  test_single_node();
  test_simple_edge();
  test_linear_chain();
  test_diamond_topology();

  // Graph execution tests.
  test_simple_execution();
  test_passthrough_execution();
  test_named_ports_execution();
  test_concatenating_batch_holder();

  // Edge structure tests.
  test_edge_comparison();
  test_edge_set_uniqueness();

  // Graph query tests.
  test_get_last_operator();
  test_get_last_operator_throws_when_empty();
  test_get_nonexistent_node();

  // KPair and EdgeConfig tests.
  test_edge_config_simple();
  test_edge_config_concatenating();
  test_edge_config_partitioned();
  test_edge_config_flow_control();
  test_kpair_accessors();
  test_kpair_with_named_ports();

  // Operator relationship tests.
  test_operator_parent_relationship();

  // Memory manager tests.
  test_graph_memory_manager();

  // Operator state tests.
  test_operator_execution_state();
  test_operator_limit_optimization();

  // Port tests.
  test_port_registration();
  test_add_ports_helper();

  std::print("\n=== All {} tests passed! ===\n", 26);
  return 0;
}
