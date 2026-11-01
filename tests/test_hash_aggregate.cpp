// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_hash_aggregate.cpp
/// @brief Tests for HashAggregateOperator: aggregate semantics, the blocking-operator
///        role contract, concurrent Sink(), and a TPC-H lineitem check against DuckDB.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/io/parquet/parquet_data_source.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "quest/quest.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/operators/hash_aggregate_operator.hpp"
#include "quest/taskflow/operators/sort_operator.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "support/test_utils.hpp"

namespace quest::taskflow {
namespace {

using cache::DataBlockPtr;
using cache::HostMemoryBlock;
using columnar::ColumnVectorPtr;
using columnar::DataType;
using columnar::Field;
using columnar::RecordBatch;
using columnar::Schema;
using plan::physical::ColumnExpression;
using plan::physical::ExpressionPtr;

// =============================================================================
// Helpers
// =============================================================================

template <typename T>
[[nodiscard]] ColumnVectorPtr MakeColumn(
    DataType type, const std::vector<std::optional<T>>& values) {
  using CType = std::conditional_t<std::is_same_v<T, std::string>, std::string_view, T>;
  columnar::ColumnBuilder<CType> col(type, values.size());
  for (const auto& v : values) {
    v ? col.Append(*v) : col.AppendNull();
  }
  return std::move(col).Finish();
}

[[nodiscard]] ColumnVectorPtr Int32s(std::vector<std::optional<int32_t>> v) {
  return MakeColumn<int32_t>(columnar::DataType::kInt32, v);
}
[[nodiscard]] ColumnVectorPtr Int64s(std::vector<std::optional<int64_t>> v) {
  return MakeColumn<int64_t>(columnar::DataType::kInt64, v);
}
[[nodiscard]] ColumnVectorPtr Doubles(std::vector<std::optional<double>> v) {
  return MakeColumn<double>(columnar::DataType::kDouble, v);
}
[[nodiscard]] ColumnVectorPtr Strings(std::vector<std::optional<std::string>> v) {
  return MakeColumn<std::string>(columnar::DataType::kString, v);
}

[[nodiscard]] RecordBatch MakeBatch(
    std::vector<std::pair<std::string, ColumnVectorPtr>> columns) {
  std::vector<Field> fields;
  std::vector<ColumnVectorPtr> cols;
  for (auto& [name, col] : columns) {
    fields.emplace_back(name, col->GetType(), columnar::Repetition::kOptional);
    cols.push_back(std::move(col));
  }
  return RecordBatch(Schema(std::move(fields)), std::move(cols));
}

[[nodiscard]] DataBlockPtr Block(RecordBatch batch) {
  return std::make_shared<HostMemoryBlock>("test_block", std::move(batch));
}

[[nodiscard]] ExpressionPtr Col(std::size_t index) {
  return std::make_unique<ColumnExpression>(index);
}

[[nodiscard]] AggregateSpec Agg(AggregateKind kind, ExpressionPtr input, std::string name) {
  return AggregateSpec{kind, std::move(input), std::move(name)};
}

/// @brief SUM, COUNT, COUNT(*), MIN, MAX, AVG over input column @p index.
[[nodiscard]] std::vector<AggregateSpec> AllAggregates(std::size_t index) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(index), "s"));
  aggs.push_back(Agg(AggregateKind::kCount, Col(index), "c"));
  aggs.push_back(Agg(AggregateKind::kCountStar, nullptr, "n"));
  aggs.push_back(Agg(AggregateKind::kMin, Col(index), "mn"));
  aggs.push_back(Agg(AggregateKind::kMax, Col(index), "mx"));
  aggs.push_back(Agg(AggregateKind::kAvg, Col(index), "a"));
  return aggs;
}

[[nodiscard]] std::vector<std::string> TypeNames(const RecordBatch& batch) {
  std::vector<std::string> names;
  for (const auto& field : batch.schema().fields()) {
    names.push_back(columnar::GetTypeName(field.data_type));
  }
  return names;
}

[[nodiscard]] std::vector<std::string> FieldNames(const RecordBatch& batch) {
  std::vector<std::string> names;
  for (const auto& field : batch.schema().fields()) {
    names.push_back(field.name);
  }
  return names;
}

[[nodiscard]] std::vector<std::string> SortedLines(const std::string& csv) {
  std::vector<std::string> lines;
  std::istringstream in(csv);
  for (std::string line; std::getline(in, line);) {
    lines.push_back(line);
  }
  std::ranges::sort(lines);
  return lines;
}

[[nodiscard]] Result<RecordBatch> HostBatch(const DataBlockPtr& block) {
  auto host = cache::EnsureHostBlock(block);
  if (!host) {
    return std::unexpected(host.error());
  }
  return *(*host)->record_batch();
}

/// @brief Sink every batch, finalize, and return the single emitted batch (nullopt if the
///        operator emitted nothing). Fails if a second batch is emitted.
[[nodiscard]] Result<std::optional<RecordBatch>> Drive(Operator& op,
                                                       const std::vector<RecordBatch>& batches) {
  for (const auto& batch : batches) {
    if (auto status = op.Sink(Block(batch)); !status) {
      return std::unexpected(status.error());
    }
  }
  if (auto status = op.FinalizeSink(); !status) {
    return std::unexpected(status.error());
  }
  auto first = op.GetNextBatch();
  if (!first) {
    return std::unexpected(first.error());
  }
  if (!first->has_value()) {
    return std::optional<RecordBatch>{};
  }
  auto second = op.GetNextBatch();
  if (!second || second->has_value()) {
    return Error::Invalid("expected end-of-stream after the single output batch");
  }
  auto batch = HostBatch(**first);
  if (!batch) {
    return std::unexpected(batch.error());
  }
  return std::optional<RecordBatch>{std::move(*batch)};
}

/// @brief Source double that yields a fixed list of batches.
class BatchListSource : public Operator {
 public:
  BatchListSource(std::uint32_t id, std::vector<RecordBatch> batches)
      : Operator(id, "batch_list_source", OperatorType::TableScan), batches_(std::move(batches)) {}

  [[nodiscard]] Result<std::optional<DataBlockPtr>> GetNextBatch() override {
    std::lock_guard lock(mutex_);
    if (next_ >= batches_.size()) {
      return std::optional<DataBlockPtr>{std::nullopt};
    }
    return std::optional<DataBlockPtr>{Block(batches_[next_++])};
  }

 private:
  std::mutex mutex_;
  std::vector<RecordBatch> batches_;
  std::size_t next_{0};
};

// =============================================================================
// Aggregate semantics
// =============================================================================

TEST(HashAggregateTest, GroupedAggregatesIgnoreNulls) {
  HashAggregateOperator op{0, {"g"}, AllAggregates(1)};
  std::vector<RecordBatch> input;
  input.push_back(MakeBatch({{"g", Int32s({1, 2, 1, 2, 1})},
                             {"v", Int32s({10, std::nullopt, 20, std::nullopt, std::nullopt})}}));
  input.push_back(MakeBatch({{"g", Int32s({2, 1})}, {"v", Int32s({5, 30})}}));

  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, input));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(FieldNames(*out), (std::vector<std::string>{"g", "s", "c", "n", "mn", "mx", "a"}));
  EXPECT_EQ(TypeNames(*out), (std::vector<std::string>{"Int32", "Int64", "Int64", "Int64", "Int32",
                                                       "Int32", "Double"}));
  EXPECT_EQ(out->ToCSV(),
            "1,60,3,4,10,30,20.000000\n"
            "2,5,1,3,5,5,5.000000\n");
}

TEST(HashAggregateTest, GroupWithOnlyNullInputsYieldsNullsAndZeroCount) {
  HashAggregateOperator op{0, {"g"}, AllAggregates(1)};
  ASSERT_OK_AND_ASSIGN(
      auto out,
      Drive(op, {MakeBatch({{"g", Int32s({7, 7})}, {"v", Int32s({std::nullopt, std::nullopt})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "7,null,0,2,null,null,null\n");
  EXPECT_TRUE(out->column(1).IsNull(0));
  EXPECT_FALSE(out->column(2).IsNull(0));
}

TEST(HashAggregateTest, NoGroupByAggregatesAllRows) {
  HashAggregateOperator op{0, {}, AllAggregates(0)};
  std::vector<RecordBatch> input;
  input.push_back(MakeBatch({{"v", Int64s({4, std::nullopt})}}));
  input.push_back(MakeBatch({{"v", Int64s({-1, 3})}}));

  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, input));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(TypeNames(*out),
            (std::vector<std::string>{"Int64", "Int64", "Int64", "Int64", "Int64", "Double"}));
  EXPECT_EQ(out->ToCSV(), "6,3,4,-1,4,2.000000\n");
}

TEST(HashAggregateTest, NoGroupByEmptyInputEmitsOneRow) {
  HashAggregateOperator op{0, {}, AllAggregates(0)};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->num_rows(), 1U);
  EXPECT_EQ(out->ToCSV(), "null,0,0,null,null,null\n");
  EXPECT_EQ(TypeNames(*out),
            (std::vector<std::string>{"Int64", "Int64", "Int64", "Int64", "Int64", "Double"}));
}

TEST(HashAggregateTest, NoGroupByZeroRowBatchEmitsOneRow) {
  HashAggregateOperator op{0, {}, AllAggregates(0)};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {MakeBatch({{"v", Int32s({})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "null,0,0,null,null,null\n");
}

TEST(HashAggregateTest, GroupByEmptyInputEmitsNothing) {
  HashAggregateOperator op{0, {"g"}, AllAggregates(1)};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {}));
  EXPECT_FALSE(out.has_value());

  HashAggregateOperator zero_rows{1, {"g"}, AllAggregates(1)};
  ASSERT_OK_AND_ASSIGN(auto out2,
                       Drive(zero_rows, {MakeBatch({{"g", Int32s({})}, {"v", Int32s({})}})}));
  EXPECT_FALSE(out2.has_value());
}

TEST(HashAggregateTest, NullGroupKeysFormOneGroup) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(1), "s"));
  HashAggregateOperator op{0, {"g"}, std::move(aggs)};
  std::vector<RecordBatch> input;
  input.push_back(MakeBatch({{"g", Strings({std::nullopt, "x"})}, {"v", Int32s({1, 2})}}));
  input.push_back(MakeBatch({{"g", Strings({std::nullopt})}, {"v", Int32s({3})}}));

  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, input));
  ASSERT_TRUE(out.has_value());
  ASSERT_EQ(out->num_rows(), 2U);
  EXPECT_TRUE(out->column(0).IsNull(0));
  EXPECT_EQ(out->ToCSV(), "null,4\nx,2\n");
}

TEST(HashAggregateTest, MultipleGroupColumnsWithStringKeysInFirstAppearanceOrder) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(2), "s"));
  aggs.push_back(Agg(AggregateKind::kCountStar, nullptr, "n"));
  HashAggregateOperator op{0, {"mode", "flag"}, std::move(aggs)};
  std::vector<RecordBatch> input;
  input.push_back(MakeBatch({{"mode", Strings({"SHIP", "AIR", "SHIP"})},
                             {"flag", Int64s({1, 1, 2})},
                             {"v", Doubles({1.5, 2.0, 3.0})}}));
  input.push_back(MakeBatch(
      {{"mode", Strings({"AIR", "SHIP"})}, {"flag", Int64s({2, 1})}, {"v", Doubles({0.25, 0.5})}}));

  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, input));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(FieldNames(*out), (std::vector<std::string>{"mode", "flag", "s", "n"}));
  EXPECT_EQ(TypeNames(*out), (std::vector<std::string>{"String", "Int64", "Double", "Int64"}));
  EXPECT_EQ(out->ToCSV(),
            "SHIP,1,2.000000,2\n"
            "AIR,1,2.000000,1\n"
            "SHIP,2,3.000000,1\n"
            "AIR,2,0.250000,1\n");
}

TEST(HashAggregateTest, DoubleInputKeepsDoubleForAllButCount) {
  HashAggregateOperator op{0, {"g"}, AllAggregates(1)};
  ASSERT_OK_AND_ASSIGN(
      auto out,
      Drive(op,
            {MakeBatch({{"g", Int32s({1, 1, 1})}, {"v", Doubles({2.5, std::nullopt, -1.0})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(TypeNames(*out), (std::vector<std::string>{"Int32", "Double", "Int64", "Int64",
                                                       "Double", "Double", "Double"}));
  EXPECT_EQ(out->ToCSV(), "1,1.500000,2,3,-1.000000,2.500000,0.750000\n");
}

TEST(HashAggregateTest, Int32SumWidensToInt64) {
  constexpr auto kMax = std::numeric_limits<int32_t>::max();
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(0), "s"));
  HashAggregateOperator op{0, {}, std::move(aggs)};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {MakeBatch({{"v", Int32s({kMax, kMax, kMax})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), std::to_string(3LL * kMax) + "\n");
}

TEST(HashAggregateTest, DecimalSumIsExactAvgIsDouble) {
  constexpr DataType kMoney = DataType::Decimal(15, 2);
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(1), "s"));
  aggs.push_back(Agg(AggregateKind::kAvg, Col(1), "a"));
  aggs.push_back(Agg(AggregateKind::kMax, Col(1), "m"));
  HashAggregateOperator op{0, {"g"}, std::move(aggs)};
  ASSERT_OK_AND_ASSIGN(
      auto out, Drive(op, {MakeBatch({{"g", Int32s({1, 1, 1})},
                                      {"v", MakeColumn<columnar::Decimal64>(
                                                kMoney, {columnar::Decimal64{10},
                                                         columnar::Decimal64{20},
                                                         std::nullopt})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(TypeNames(*out),
            (std::vector<std::string>{"Int32", "Decimal(18,2)", "Double", "Decimal(15,2)"}));
  EXPECT_EQ(out->ToCSV(), "1,0.30,0.150000,0.20\n");
}

TEST(HashAggregateTest, DecimalSumOverflowIsOutOfRange) {
  constexpr DataType kWide = DataType::Decimal(18, 0);
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(0), "s"));
  HashAggregateOperator op{0, {}, std::move(aggs)};
  auto status = op.Sink(Block(MakeBatch(
      {{"v", MakeColumn<columnar::Decimal64>(
                 kWide, {columnar::Decimal64{std::numeric_limits<int64_t>::max()},
                         columnar::Decimal64{1}})}})));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
}

TEST(HashAggregateTest, StringMinMaxCompareBytewise) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kMin, Col(0), "mn"));
  aggs.push_back(Agg(AggregateKind::kMax, Col(0), "mx"));
  aggs.push_back(Agg(AggregateKind::kCount, Col(0), "c"));
  HashAggregateOperator op{0, {}, std::move(aggs)};
  ASSERT_OK_AND_ASSIGN(
      auto out,
      Drive(op, {MakeBatch({{"s", Strings({"b", "B", std::nullopt, "a", "\xC3\xA9"})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(TypeNames(*out), (std::vector<std::string>{"String", "String", "Int64"}));
  EXPECT_EQ(out->ToCSV(), "B,\xC3\xA9,4\n");
}

TEST(HashAggregateTest, AggregateInputIsAnExpression) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(
      AggregateKind::kSum,
      std::make_unique<plan::physical::BinaryExpression>(plan::BinaryOp::kMultiply, Col(1), Col(2)),
      "revenue"));
  HashAggregateOperator op{0, {"g"}, std::move(aggs)};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {MakeBatch({{"g", Strings({"a", "b", "a"})},
                                                       {"x", Int64s({2, 3, 4})},
                                                       {"y", Int64s({10, 10, 100})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "a,420\nb,30\n");
}

// =============================================================================
// Errors
// =============================================================================

TEST(HashAggregateTest, IntegerSumOverflowWithinBatchIsOutOfRange) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(0), "s"));
  HashAggregateOperator op{0, {}, std::move(aggs)};
  auto status =
      op.Sink(Block(MakeBatch({{"v", Int64s({std::numeric_limits<int64_t>::max(), 1})}})));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
}

TEST(HashAggregateTest, IntegerSumOverflowAcrossBatchesIsOutOfRange) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(0), "s"));
  HashAggregateOperator op{0, {}, std::move(aggs)};
  ASSERT_OK(op.Sink(Block(MakeBatch({{"v", Int64s({std::numeric_limits<int64_t>::min()})}}))));
  auto status = op.Sink(Block(MakeBatch({{"v", Int64s({-1})}})));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::OutOfRange);
}

TEST(HashAggregateTest, MissingGroupColumnIsSchemaError) {
  HashAggregateOperator op{0, {"nope"}, AllAggregates(0)};
  auto status = op.Sink(Block(MakeBatch({{"v", Int32s({1})}})));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::SchemaError);
}

TEST(HashAggregateTest, SumOfStringIsTypeError) {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(0), "s"));
  HashAggregateOperator op{0, {}, std::move(aggs)};
  auto status = op.Sink(Block(MakeBatch({{"v", Strings({"x"})}})));
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::TypeError);
}

// =============================================================================
// Pipeline-role contract
// =============================================================================

TEST(HashAggregateTest, RoleContract) {
  HashAggregateOperator op{0, {"g"}, AllAggregates(1)};
  EXPECT_EQ(op.type(), OperatorType::HashAggregate);
  EXPECT_TRUE(op.IsBlocking());
  EXPECT_TRUE(op.IsSink());
  EXPECT_FALSE(op.IsSource());
  EXPECT_FALSE(op.GetNextBatch().has_value());

  ASSERT_OK(op.Sink(Block(MakeBatch({{"g", Int32s({1})}, {"v", Int32s({1})}}))));
  EXPECT_FALSE(op.IsSource());

  ASSERT_OK(op.FinalizeSink());
  EXPECT_TRUE(op.IsSource());
  EXPECT_FALSE(op.FinalizeSink().has_value());
  EXPECT_FALSE(op.Sink(Block(MakeBatch({{"g", Int32s({1})}, {"v", Int32s({1})}}))).has_value());

  ASSERT_OK_AND_ASSIGN(auto first, op.GetNextBatch());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)->num_rows(), 1U);
  for (int i = 0; i < 2; ++i) {
    ASSERT_OK_AND_ASSIGN(auto eos, op.GetNextBatch());
    EXPECT_FALSE(eos.has_value());
  }
}

// =============================================================================
// Thread safety
// =============================================================================

[[nodiscard]] std::vector<RecordBatch> ManyBatches(std::size_t count) {
  std::vector<RecordBatch> batches;
  for (std::size_t b = 0; b < count; ++b) {
    std::vector<std::optional<std::string>> keys;
    std::vector<std::optional<int64_t>> ints;
    std::vector<std::optional<double>> doubles;
    for (std::size_t r = 0; r < 64; ++r) {
      const auto i = static_cast<int64_t>(b * 64 + r);
      keys.emplace_back(i % 17 == 0 ? std::nullopt
                                    : std::optional<std::string>("k" + std::to_string(i % 13)));
      ints.emplace_back(i % 5 == 0 ? std::nullopt : std::optional<int64_t>(i - 3000));
      doubles.emplace_back(static_cast<double>(i % 7));
    }
    batches.push_back(
        MakeBatch({{"k", Strings(keys)}, {"i", Int64s(ints)}, {"d", Doubles(doubles)}}));
  }
  return batches;
}

[[nodiscard]] std::vector<AggregateSpec> MixedAggregates() {
  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kSum, Col(1), "sum_i"));
  aggs.push_back(Agg(AggregateKind::kSum, Col(2), "sum_d"));
  aggs.push_back(Agg(AggregateKind::kCount, Col(1), "count_i"));
  aggs.push_back(Agg(AggregateKind::kCountStar, nullptr, "count_star"));
  aggs.push_back(Agg(AggregateKind::kMin, Col(1), "min_i"));
  aggs.push_back(Agg(AggregateKind::kMax, Col(1), "max_i"));
  aggs.push_back(Agg(AggregateKind::kAvg, Col(2), "avg_d"));
  return aggs;
}

TEST(HashAggregateTest, ConcurrentSinkMatchesSingleThreaded) {
  const auto batches = ManyBatches(200);

  HashAggregateOperator reference{0, {"k"}, MixedAggregates()};
  ASSERT_OK_AND_ASSIGN(auto expected, Drive(reference, batches));
  ASSERT_TRUE(expected.has_value());

  HashAggregateOperator op{1, {"k"}, MixedAggregates()};
  constexpr std::size_t kThreads = 8;
  std::vector<Status> statuses(kThreads);
  std::vector<std::thread> threads;
  for (std::size_t t = 0; t < kThreads; ++t) {
    threads.emplace_back([&, t] {
      for (std::size_t b = t; b < batches.size(); b += kThreads) {
        if (auto status = op.Sink(Block(batches[b])); !status) {
          statuses[t] = status;
          return;
        }
      }
    });
  }
  for (auto& thread : threads) {
    thread.join();
  }
  for (const auto& status : statuses) {
    ASSERT_OK(status);
  }
  ASSERT_OK_AND_ASSIGN(auto actual, Drive(op, {}));
  ASSERT_TRUE(actual.has_value());
  EXPECT_EQ(SortedLines(actual->ToCSV()), SortedLines(expected->ToCSV()));
}

TEST(HashAggregateTest, PipelineExecutorWithWorkersMatchesSingleThreaded) {
  const auto batches = ManyBatches(300);

  HashAggregateOperator reference{0, {"k"}, MixedAggregates()};
  ASSERT_OK_AND_ASSIGN(auto expected, Drive(reference, batches));
  ASSERT_TRUE(expected.has_value());

  BatchListSource source{0, batches};
  HashAggregateOperator agg{1, {"k"}, MixedAggregates()};
  CollectorOperator collector{2};
  GraphBuilder gb;
  gb.Chain(source, agg, collector);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  ASSERT_EQ(pipelines.size(), 2U);

  PipelineExecutor executor{4};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  ASSERT_OK_AND_ASSIGN(auto collected, collector.CollectBatches());
  ASSERT_EQ(collected.size(), 1U);
  EXPECT_EQ(SortedLines(collected[0].ToCSV()), SortedLines(expected->ToCSV()));
}

// =============================================================================
// TPC-H lineitem: GROUP BY l_shipmode, then ORDER BY l_shipmode
// =============================================================================

[[nodiscard]] std::filesystem::path LineitemParquet() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "tpch" /
         "lineitem.parquet";
}

// Expected rows from:
//   duckdb -c "select l_shipmode, count(*), min(l_orderkey)
//              from 'testdata/tpch/lineitem.parquet' group by l_shipmode order by l_shipmode"
TEST(HashAggregateTest, LineitemShipmodeAggregateThenSortMatchesDuckDB) {
  if (!std::filesystem::exists(LineitemParquet())) {
    GTEST_SKIP() << "missing " << LineitemParquet();
  }
  ASSERT_OK_AND_ASSIGN(auto opened,
                       io::parquet::ParquetDataSource::Open(LineitemParquet().string()));
  auto data_source = std::make_shared<io::parquet::ParquetDataSource>(std::move(opened));
  auto orderkey_index = data_source->schema().GetFieldIndex("l_orderkey");
  ASSERT_TRUE(orderkey_index.has_value());
  auto memory_manager = make_default_memory_manager();

  std::vector<AggregateSpec> aggs;
  aggs.push_back(Agg(AggregateKind::kCountStar, nullptr, "count_star"));
  aggs.push_back(Agg(AggregateKind::kMin, Col(*orderkey_index), "min_orderkey"));

  TableScanOperator scan{0, data_source, std::nullopt, memory_manager};
  HashAggregateOperator agg{1, {"l_shipmode"}, std::move(aggs), memory_manager};
  SortOperator sort{2, {SortKey{"l_shipmode", true}}, memory_manager};
  CollectorOperator collector{3};

  GraphBuilder gb{memory_manager};
  gb.Chain(scan, agg, sort, collector);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  ASSERT_EQ(pipelines.size(), 3U);

  PipelineExecutor executor{4};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  ASSERT_OK_AND_ASSIGN(auto collected, collector.CollectBatches());
  ASSERT_EQ(collected.size(), 1U);
  EXPECT_EQ(FieldNames(collected[0]),
            (std::vector<std::string>{"l_shipmode", "count_star", "min_orderkey"}));
  EXPECT_EQ(TypeNames(collected[0]), (std::vector<std::string>{"String", "Int64", "Int64"}));
  EXPECT_EQ(collected[0].ToCSV(),
            "AIR,8491,1\n"
            "FOB,8641,1\n"
            "MAIL,8669,1\n"
            "RAIL,8566,2\n"
            "REG AIR,8616,1\n"
            "SHIP,8482,3\n"
            "TRUCK,8710,1\n");
}

}  // namespace
}  // namespace quest::taskflow
