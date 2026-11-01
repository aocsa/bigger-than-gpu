// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_hash_join.cpp
/// @brief HashJoinOperator: port roles, MetaPipeline shape, executor finalize
///        ordering, join correctness, and a TPC-H build/probe join.

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/io/parquet/parquet_data_source.hpp"
#include "quest/plan/binary_op.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/operators/filter_operator.hpp"
#include "quest/taskflow/operators/hash_join_operator.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::columnar;
using quest::cache::DataBlockPtr;

namespace {

using OptInt = std::optional<std::int64_t>;

/// @brief Int64 (or Int32 when @p as_int32) column; nullopt entries are null.
[[nodiscard]] std::shared_ptr<ColumnVector> IntColumn(const std::vector<OptInt>& values,
                                                      bool as_int32 = false) {
  std::vector<bool> validity;
  bool has_null = false;
  for (const auto& value : values) {
    validity.push_back(value.has_value());
    has_null |= !value.has_value();
  }
  if (!has_null) {
    validity.clear();
  }
  if (as_int32) {
    std::vector<std::int32_t> data;
    for (const auto& value : values) {
      data.push_back(static_cast<std::int32_t>(value.value_or(0)));
    }
    return std::make_shared<plan::physical::MaterializedColumnVector>(
        plan::physical::ColumnData{std::move(data)}, Int32Type{}, std::move(validity));
  }
  std::vector<std::int64_t> data;
  for (const auto& value : values) {
    data.push_back(value.value_or(0));
  }
  return std::make_shared<plan::physical::MaterializedColumnVector>(
      plan::physical::ColumnData{std::move(data)}, Int64Type{}, std::move(validity));
}

[[nodiscard]] std::shared_ptr<ColumnVector> StringColumn(std::vector<std::string> values) {
  return std::make_shared<plan::physical::MaterializedColumnVector>(
      plan::physical::ColumnData{std::move(values)}, StringType{});
}

/// @brief Two-column block: an integer key column and a string payload column.
[[nodiscard]] DataBlockPtr KeyPayloadBlock(const std::string& key_name,
                                           const std::string& payload_name,
                                           const std::vector<OptInt>& keys,
                                           std::vector<std::string> payloads,
                                           bool key_as_int32 = false) {
  Schema schema({Field(key_name, key_as_int32 ? DataType{Int32Type{}} : DataType{Int64Type{}}),
                 Field(payload_name, StringType{})});
  RecordBatch batch(std::move(schema),
                    {IntColumn(keys, key_as_int32), StringColumn(std::move(payloads))});
  return cache::MakeTrackedHostBlock("test_block", std::move(batch), nullptr);
}

[[nodiscard]] const RecordBatch& BatchOf(const DataBlockPtr& block) {
  return *std::dynamic_pointer_cast<cache::HostMemoryBlock>(block)->record_batch();
}

/// @brief Rows of @p batch rendered as "v0|v1|...", one string per row.
[[nodiscard]] std::vector<std::string> Rows(const RecordBatch& batch) {
  std::vector<std::string> rows;
  for (std::size_t row = 0; row < batch.num_rows(); ++row) {
    std::string rendered;
    for (std::size_t col = 0; col < batch.num_columns(); ++col) {
      if (col > 0) {
        rendered += "|";
      }
      rendered += ValueToString(batch.column(col).GetValue(row));
    }
    rows.push_back(std::move(rendered));
  }
  return rows;
}

[[nodiscard]] std::vector<std::string> FieldNames(const Schema& schema) {
  std::vector<std::string> names;
  for (const auto& field : schema.fields()) {
    names.push_back(field.name);
  }
  return names;
}

/// @brief Source that hands out a fixed list of blocks.
class BlockSource : public Operator {
 public:
  BlockSource(std::uint32_t id, std::vector<DataBlockPtr> blocks)
      : Operator(id, "block_source", OperatorType::TableScan), blocks_(std::move(blocks)) {}

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    if (next_ >= blocks_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    return std::optional<DataBlockPtr>{blocks_[next_++]};
  }

 private:
  std::vector<DataBlockPtr> blocks_;
  std::size_t next_{0};
};

/// @brief Streaming passthrough (stands in for the build-side filter in shape tests).
class PassthroughOperator : public Operator {
 public:
  explicit PassthroughOperator(std::uint32_t id)
      : Operator(id, "passthrough", OperatorType::Filter) {}
};

/// @brief Terminal sink without a Full input contract, so it fuses into the
///        probe pipeline (stands in for a grouped aggregate's sink role).
class FusingSink : public Operator {
 public:
  explicit FusingSink(std::uint32_t id) : Operator(id, "fusing_sink", OperatorType::Output) {}

  [[nodiscard]] bool IsSink() const noexcept override { return true; }

  [[nodiscard]] Status Sink(DataBlockPtr batch) override {
    std::lock_guard lock(mutex_);
    batches_.push_back(std::move(batch));
    return {};
  }

  [[nodiscard]] const std::vector<DataBlockPtr>& batches() const noexcept { return batches_; }

 private:
  std::mutex mutex_;
  std::vector<DataBlockPtr> batches_;
};

/// @brief Multi-input operator with two fused ports (invalid shape).
class TwoFusedPortsOperator : public Operator {
 public:
  explicit TwoFusedPortsOperator(std::uint32_t id)
      : Operator(id, "two_fused", OperatorType::HashJoin) {}

  [[nodiscard]] std::vector<std::string_view> DeclaredInputPorts() const override {
    return {"left", "right"};
  }

  [[nodiscard]] PortRole InputPortRole(std::string_view /*port*/) const override {
    return PortRole::kFused;
  }
};

[[nodiscard]] std::filesystem::path TpchDir() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "tpch";
}

/// @brief Runs the build side and then probes @p probe_blocks directly.
[[nodiscard]] Result<std::vector<std::string>> JoinRows(HashJoinOperator& join,
                                                        const std::vector<DataBlockPtr>& build,
                                                        const DataBlockPtr& probe) {
  for (const auto& block : build) {
    auto status = join.Sink(block);
    if (!status.has_value()) {
      return std::unexpected(status.error());
    }
  }
  auto finalized = join.FinalizeSink();
  if (!finalized.has_value()) {
    return std::unexpected(finalized.error());
  }
  auto output = join.ProcessBatch(probe);
  if (!output.has_value()) {
    return std::unexpected(output.error());
  }
  return Rows(BatchOf(*output));
}

}  // namespace

// =============================================================================
// Operator contract
// =============================================================================

TEST(HashJoinOperatorTest, PortContract) {
  HashJoinOperator join{7, "l_orderkey", "o_orderkey"};

  EXPECT_EQ(join.type(), OperatorType::HashJoin);
  EXPECT_FALSE(join.IsBlocking());
  EXPECT_TRUE(join.IsSink());
  EXPECT_FALSE(join.IsSource());
  EXPECT_EQ(join.DeclaredInputPorts(), (std::vector<std::string_view>{"build", "probe"}));
  EXPECT_EQ(join.InputPortRole("build"), PortRole::kSink);
  EXPECT_EQ(join.InputPortRole("probe"), PortRole::kFused);
  EXPECT_EQ(join.build(), (OperatorPortPair{&join, "build"}));
  EXPECT_EQ(join.probe(), (OperatorPortPair{&join, "probe"}));
}

TEST(HashJoinOperatorTest, DefaultPortRoleIsHolder) {
  PassthroughOperator op{1};
  EXPECT_EQ(op.InputPortRole("anything"), PortRole::kHolder);
}

// =============================================================================
// Join semantics (operator driven directly)
// =============================================================================

TEST(HashJoinOperatorTest, DuplicatesKeepProbeOrderThenBuildInsertionOrder) {
  HashJoinOperator join{0, "bk", "pk"};
  auto build_a = KeyPayloadBlock("bk", "bv", {1, 2, 1}, {"b0", "b1", "b2"});
  auto build_b = KeyPayloadBlock("bk", "bv", {3, 1}, {"b3", "b4"});
  auto probe = KeyPayloadBlock("pk", "pv", {1, 4, 3, 1, 2}, {"p0", "p1", "p2", "p3", "p4"});

  ASSERT_OK_AND_ASSIGN(auto rows, JoinRows(join, {build_a, build_b}, probe));
  EXPECT_EQ(rows, (std::vector<std::string>{"1|p0|1|b0", "1|p0|1|b2", "1|p0|1|b4", "3|p2|3|b3",
                                            "1|p3|1|b0", "1|p3|1|b2", "1|p3|1|b4", "2|p4|2|b1"}));

  ASSERT_OK_AND_ASSIGN(auto output, join.ProcessBatch(probe));
  EXPECT_EQ(FieldNames(BatchOf(output).schema()),
            (std::vector<std::string>{"pk", "pv", "bk", "bv"}));
}

TEST(HashJoinOperatorTest, NullKeysNeverMatch) {
  HashJoinOperator join{0, "bk", "pk"};
  auto build = KeyPayloadBlock("bk", "bv", {std::nullopt, 5, std::nullopt}, {"bn0", "b5", "bn2"});
  auto probe = KeyPayloadBlock("pk", "pv", {std::nullopt, 5}, {"pn", "p5"});

  ASSERT_OK_AND_ASSIGN(auto rows, JoinRows(join, {build}, probe));
  EXPECT_EQ(rows, (std::vector<std::string>{"5|p5|5|b5"}));
}

TEST(HashJoinOperatorTest, Int32ProbeKeyMatchesInt64BuildKey) {
  HashJoinOperator join{0, "bk", "pk"};
  auto build = KeyPayloadBlock("bk", "bv", {10, 20}, {"b10", "b20"});
  auto probe = KeyPayloadBlock("pk", "pv", {20, 30, 10}, {"p20", "p30", "p10"},
                               /*key_as_int32=*/true);

  ASSERT_OK_AND_ASSIGN(auto rows, JoinRows(join, {build}, probe));
  EXPECT_EQ(rows, (std::vector<std::string>{"20|p20|20|b20", "10|p10|10|b10"}));
}

TEST(HashJoinOperatorTest, Int32BuildKeyMatchesInt64ProbeKey) {
  HashJoinOperator join{0, "bk", "pk"};
  auto build = KeyPayloadBlock("bk", "bv", {-3, 7}, {"bm3", "b7"}, /*key_as_int32=*/true);
  auto probe = KeyPayloadBlock("pk", "pv", {7, -3}, {"p7", "pm3"});

  ASSERT_OK_AND_ASSIGN(auto rows, JoinRows(join, {build}, probe));
  EXPECT_EQ(rows, (std::vector<std::string>{"7|p7|7|b7", "-3|pm3|-3|bm3"}));
}

TEST(HashJoinOperatorTest, EmptyBuildYieldsEmptyBatches) {
  HashJoinOperator join{0, "bk", "pk"};
  auto probe = KeyPayloadBlock("pk", "pv", {1, 2}, {"p1", "p2"});

  ASSERT_OK_AND_ASSIGN(auto rows, JoinRows(join, {}, probe));
  EXPECT_TRUE(rows.empty());
}

TEST(HashJoinOperatorTest, EmptyProbeBatchYieldsEmptyBatch) {
  HashJoinOperator join{0, "bk", "pk"};
  auto build = KeyPayloadBlock("bk", "bv", {1}, {"b1"});
  auto probe = KeyPayloadBlock("pk", "pv", {}, {});

  ASSERT_OK_AND_ASSIGN(auto rows, JoinRows(join, {build}, probe));
  EXPECT_TRUE(rows.empty());
}

TEST(HashJoinOperatorTest, NoMatchesYieldsZeroRowBatchWithJoinedSchema) {
  HashJoinOperator join{0, "bk", "pk"};
  auto build = KeyPayloadBlock("bk", "bv", {1}, {"b1"});
  auto probe = KeyPayloadBlock("pk", "pv", {2, 3}, {"p2", "p3"});

  ASSERT_OK(join.Sink(build));
  ASSERT_OK(join.FinalizeSink());
  ASSERT_OK_AND_ASSIGN(auto output, join.ProcessBatch(probe));
  EXPECT_EQ(output->num_rows(), 0U);
  EXPECT_EQ(FieldNames(BatchOf(output).schema()),
            (std::vector<std::string>{"pk", "pv", "bk", "bv"}));
}

TEST(HashJoinOperatorTest, MissingBuildKeyIsSchemaError) {
  HashJoinOperator join{0, "missing", "pk"};
  ASSERT_OK(join.Sink(KeyPayloadBlock("bk", "bv", {1}, {"b1"})));
  auto status = join.FinalizeSink();
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::SchemaError);
}

TEST(HashJoinOperatorTest, MissingProbeKeyIsSchemaError) {
  HashJoinOperator join{0, "bk", "missing"};
  ASSERT_OK(join.Sink(KeyPayloadBlock("bk", "bv", {1}, {"b1"})));
  ASSERT_OK(join.FinalizeSink());
  auto output = join.ProcessBatch(KeyPayloadBlock("pk", "pv", {1}, {"p1"}));
  ASSERT_FALSE(output.has_value());
  EXPECT_EQ(output.error().code(), ErrorCode::SchemaError);
}

TEST(HashJoinOperatorTest, ProbeBeforeFinalizeIsAnError) {
  HashJoinOperator join{0, "bk", "pk"};
  ASSERT_OK(join.Sink(KeyPayloadBlock("bk", "bv", {1}, {"b1"})));
  EXPECT_FALSE(join.ProcessBatch(KeyPayloadBlock("pk", "pv", {1}, {"p1"})).has_value());
}

// =============================================================================
// MetaPipeline shape
// =============================================================================

TEST(HashJoinMetaPipelineTest, BuildAndProbeSplitIntoTwoPipelines) {
  BlockSource build_scan{0, {}};
  PassthroughOperator build_filter{1};
  HashJoinOperator join{2, "bk", "pk"};
  BlockSource probe_scan{3, {}};
  FusingSink sink{4};

  GraphBuilder gb;
  gb.Edge(build_scan, build_filter);
  gb.Edge(build_filter, join.build());
  gb.Edge(probe_scan, join.probe());
  gb.Edge(join, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  ASSERT_EQ(pipelines.size(), 2U);
  const auto& build_pipeline = pipelines[0];
  const auto& probe_pipeline = pipelines[1];

  EXPECT_EQ(build_pipeline->operators(),
            (std::vector<Operator*>{&build_scan, &build_filter, &join}));
  EXPECT_TRUE(build_pipeline->FullBarrierProducers().empty());
  EXPECT_TRUE(build_pipeline->SinkAccumulates());

  EXPECT_EQ(probe_pipeline->operators(), (std::vector<Operator*>{&probe_scan, &join, &sink}));
  ASSERT_EQ(probe_pipeline->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(probe_pipeline->FullBarrierProducers()[0], build_pipeline.get());
  EXPECT_TRUE(probe_pipeline->PipelineBarrierProducers().empty());
}

TEST(HashJoinMetaPipelineTest, FullBarrierCollectorEndsProbePipelineAtTheJoin) {
  BlockSource build_scan{0, {}};
  HashJoinOperator join{1, "bk", "pk"};
  BlockSource probe_scan{2, {}};
  CollectorOperator collector{3};

  GraphBuilder gb;
  gb.Edge(build_scan, join.build());
  gb.Edge(probe_scan, join.probe());
  gb.Edge(join, collector);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  ASSERT_EQ(pipelines.size(), 3U);
  EXPECT_EQ(pipelines[0]->operators(), (std::vector<Operator*>{&build_scan, &join}));
  EXPECT_TRUE(pipelines[0]->SinkAccumulates());

  EXPECT_EQ(pipelines[1]->operators(), (std::vector<Operator*>{&probe_scan, &join}));
  EXPECT_FALSE(pipelines[1]->SinkAccumulates());
  ASSERT_EQ(pipelines[1]->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(pipelines[1]->FullBarrierProducers()[0], pipelines[0].get());

  EXPECT_EQ(pipelines[2]->operators(), (std::vector<Operator*>{&collector}));
  ASSERT_EQ(pipelines[2]->FullBarrierProducers().size(), 1U);
  EXPECT_EQ(pipelines[2]->FullBarrierProducers()[0], pipelines[1].get());
}

TEST(HashJoinMetaPipelineTest, TerminalJoinIsInvalid) {
  BlockSource build_scan{0, {}};
  HashJoinOperator join{1, "bk", "pk"};
  BlockSource probe_scan{2, {}};

  GraphBuilder gb;
  gb.Edge(build_scan, join.build());
  gb.Edge(probe_scan, join.probe());
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto pipelines = MetaPipeline::Build(*graph);
  ASSERT_FALSE(pipelines.has_value());
  EXPECT_EQ(pipelines.error().code(), ErrorCode::Invalid);
}

TEST(HashJoinMetaPipelineTest, JoinWithoutProbeProducerIsInvalid) {
  BlockSource build_scan{0, {}};
  HashJoinOperator join{1, "bk", "pk"};
  FusingSink sink{2};

  GraphBuilder gb;
  gb.Edge(build_scan, join.build());
  gb.Edge(join, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto pipelines = MetaPipeline::Build(*graph);
  ASSERT_FALSE(pipelines.has_value());
  EXPECT_EQ(pipelines.error().code(), ErrorCode::Invalid);
}

TEST(HashJoinMetaPipelineTest, TwoFusedPortsAreInvalid) {
  BlockSource left{0, {}};
  BlockSource right{1, {}};
  TwoFusedPortsOperator op{2};
  FusingSink sink{3};

  GraphBuilder gb;
  gb.Edge(left, op["left"]);
  gb.Edge(right, op["right"]);
  gb.Edge(op, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());

  auto pipelines = MetaPipeline::Build(*graph);
  ASSERT_FALSE(pipelines.has_value());
  EXPECT_EQ(pipelines.error().code(), ErrorCode::Invalid);
}

// =============================================================================
// Executor: build finalized before probe, across worker counts
// =============================================================================

class HashJoinExecutorTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(HashJoinExecutorTest, ProbeRunsAfterBuildIsFinalized) {
  std::vector<DataBlockPtr> build_blocks;
  std::vector<DataBlockPtr> probe_blocks;
  std::vector<std::string> expected;
  for (std::int64_t b = 0; b < 8; ++b) {
    build_blocks.push_back(
        KeyPayloadBlock("bk", "bv", {b * 2, b * 2 + 1},
                        {"b" + std::to_string(b * 2), "b" + std::to_string(b * 2 + 1)}));
  }
  for (std::int64_t p = 0; p < 20; ++p) {
    probe_blocks.push_back(KeyPayloadBlock("pk", "pv", {p, std::nullopt}, {"p", "n"}));
    if (p < 16) {
      expected.push_back(std::to_string(p) + "|p|" + std::to_string(p) + "|b" + std::to_string(p));
    }
  }

  BlockSource build_scan{0, build_blocks};
  PassthroughOperator build_filter{1};
  HashJoinOperator join{2, "bk", "pk"};
  BlockSource probe_scan{3, probe_blocks};
  FusingSink sink{4};

  GraphBuilder gb;
  gb.Edge(build_scan, build_filter);
  gb.Edge(build_filter, join.build());
  gb.Edge(probe_scan, join.probe());
  gb.Edge(join, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();

  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  PipelineExecutor executor{GetParam()};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  std::vector<std::string> rows;
  for (const auto& block : sink.batches()) {
    for (auto& row : Rows(BatchOf(block))) {
      rows.push_back(std::move(row));
    }
  }
  EXPECT_EQ(rows, expected);
}

TEST_P(HashJoinExecutorTest, FinalizeErrorPropagatesAndProbeNeverRuns) {
  BlockSource build_scan{0, {KeyPayloadBlock("bk", "bv", {1}, {"b1"})}};
  HashJoinOperator join{1, "missing_key", "pk"};
  BlockSource probe_scan{2, {KeyPayloadBlock("pk", "pv", {1}, {"p1"})}};
  FusingSink sink{3};

  GraphBuilder gb;
  gb.Edge(build_scan, join.build());
  gb.Edge(probe_scan, join.probe());
  gb.Edge(join, sink);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();

  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  PipelineExecutor executor{GetParam()};
  auto status = executor.Execute(std::move(pipelines));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::SchemaError);
  EXPECT_TRUE(sink.batches().empty());
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, HashJoinExecutorTest, ::testing::Values(1, 2, 4, 8));

// =============================================================================
// End to end: TPC-H orders JOIN lineitem (MAIL/SHIP)
// =============================================================================

class HashJoinTpchTest : public ::testing::TestWithParam<std::size_t> {};

TEST_P(HashJoinTpchTest, OrdersJoinFilteredLineitemMatchesDuckDb) {
  // duckdb: select count(*), sum(o_orderkey) from orders o join lineitem l
  //         on o_orderkey = l_orderkey where l_shipmode in ('MAIL','SHIP')
  constexpr std::size_t kExpectedRows = 17151;
  constexpr std::int64_t kExpectedOrderKeySum = 513326060;

  const auto lineitem_path = TpchDir() / "lineitem.parquet";
  const auto orders_path = TpchDir() / "orders.parquet";
  if (!std::filesystem::exists(lineitem_path) || !std::filesystem::exists(orders_path)) {
    GTEST_SKIP() << "missing TPC-H test data under " << TpchDir();
  }
  ASSERT_OK_AND_ASSIGN(auto lineitem_opened,
                       io::parquet::ParquetDataSource::Open(lineitem_path.string()));
  ASSERT_OK_AND_ASSIGN(auto orders_opened,
                       io::parquet::ParquetDataSource::Open(orders_path.string()));
  auto lineitem = std::make_shared<io::parquet::ParquetDataSource>(std::move(lineitem_opened));
  auto orders = std::make_shared<io::parquet::ParquetDataSource>(std::move(orders_opened));

  // Projected lineitem columns: 0 = l_orderkey, 1 = l_shipmode.
  auto shipmode_is = [](std::string mode) {
    return std::make_unique<plan::physical::BinaryExpression>(
        plan::BinaryOp::kEq, std::make_unique<plan::physical::ColumnExpression>(1),
        std::make_unique<plan::physical::LiteralExpression>(Value{std::move(mode)}));
  };
  auto predicate = std::make_unique<plan::physical::BinaryExpression>(
      plan::BinaryOp::kOr, shipmode_is("MAIL"), shipmode_is("SHIP"));

  TableScanOperator lineitem_scan{0, lineitem,
                                  std::vector<std::string>{"l_orderkey", "l_shipmode"}};
  FilterOperator shipmode_filter{1, std::move(predicate)};
  HashJoinOperator join{2, "l_orderkey", "o_orderkey"};
  TableScanOperator orders_scan{3, orders};
  CollectorOperator collector{4};

  GraphBuilder gb;
  gb.Edge(lineitem_scan, shipmode_filter);
  gb.Edge(shipmode_filter, join.build());
  gb.Edge(orders_scan, join.probe());
  gb.Edge(join, collector);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();

  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  PipelineExecutor executor{GetParam()};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  EXPECT_EQ(collector.total_rows(), kExpectedRows);
  ASSERT_OK_AND_ASSIGN(auto batches, collector.CollectBatches());
  std::int64_t order_key_sum = 0;
  for (const auto& batch : batches) {
    ASSERT_EQ(FieldNames(batch.schema()), (std::vector<std::string>{"o_orderkey", "o_orderpriority",
                                                                    "l_orderkey", "l_shipmode"}));
    for (std::size_t row = 0; row < batch.num_rows(); ++row) {
      auto order_key = std::get<std::int64_t>(batch.column(0).GetValue(row));
      EXPECT_EQ(order_key, std::get<std::int64_t>(batch.column(2).GetValue(row)));
      auto mode = std::get<std::string>(batch.column(3).GetValue(row));
      EXPECT_TRUE(mode == "MAIL" || mode == "SHIP") << mode;
      order_key_sum += order_key;
    }
  }
  EXPECT_EQ(order_key_sum, kExpectedOrderKeySum);
}

INSTANTIATE_TEST_SUITE_P(WorkerCounts, HashJoinTpchTest, ::testing::Values(1, 2, 8));
