// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_sort.cpp
/// @brief Tests for SortOperator: key ordering, null placement, stability, the
///        blocking-operator role contract, and concurrent Sink().

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/taskflow/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/collector_operator.hpp"
#include "quest/taskflow/operators/sort_operator.hpp"
#include "quest/taskflow/pipeline_executor.hpp"
#include "support/test_utils.hpp"

namespace quest::taskflow {
namespace {

using cache::DataBlockPtr;
using cache::HostMemoryBlock;
using columnar::ColumnVector;
using columnar::DataType;
using columnar::Field;
using columnar::RecordBatch;
using columnar::Schema;

// =============================================================================
// Helpers
// =============================================================================

template <typename T>
[[nodiscard]] std::shared_ptr<ColumnVector> MakeColumn(
    DataType type, const std::vector<std::optional<T>>& values) {
  if constexpr (std::is_same_v<T, std::string>) {
    auto col = std::make_shared<columnar::StringColumnVector>(type, values.size());
    for (const auto& v : values) {
      v ? col->Append(*v) : col->AppendNull();
    }
    return col;
  } else {
    auto col = std::make_shared<columnar::TypedColumnVector<T>>(type, values.size());
    for (const auto& v : values) {
      v ? col->Append(*v) : col->AppendNull();
    }
    return col;
  }
}

[[nodiscard]] std::shared_ptr<ColumnVector> Int32s(std::vector<std::optional<int32_t>> v) {
  return MakeColumn<int32_t>(columnar::DataType::kInt32, v);
}
[[nodiscard]] std::shared_ptr<ColumnVector> Int64s(std::vector<std::optional<int64_t>> v) {
  return MakeColumn<int64_t>(columnar::DataType::kInt64, v);
}
[[nodiscard]] std::shared_ptr<ColumnVector> Doubles(std::vector<std::optional<double>> v) {
  return MakeColumn<double>(columnar::DataType::kDouble, v);
}
[[nodiscard]] std::shared_ptr<ColumnVector> Strings(std::vector<std::optional<std::string>> v) {
  return MakeColumn<std::string>(columnar::DataType::kString, v);
}

[[nodiscard]] RecordBatch MakeBatch(
    std::vector<std::pair<std::string, std::shared_ptr<ColumnVector>>> columns) {
  std::vector<Field> fields;
  std::vector<std::shared_ptr<ColumnVector>> cols;
  for (auto& [name, col] : columns) {
    fields.emplace_back(name, col->GetType(), columnar::Repetition::kOptional);
    cols.push_back(std::move(col));
  }
  return RecordBatch(Schema(std::move(fields)), std::move(cols));
}

[[nodiscard]] DataBlockPtr Block(RecordBatch batch) {
  return std::make_shared<HostMemoryBlock>("test_block", std::move(batch));
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
  auto host = cache::EnsureHostBlock(**first);
  if (!host) {
    return std::unexpected(host.error());
  }
  return std::optional<RecordBatch>{*(*host)->record_batch()};
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
// Ordering
// =============================================================================

TEST(SortTest, AscendingPutsNullsLastAndCarriesOtherColumns) {
  SortOperator op{0, {SortKey{"x", true}}};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {MakeBatch({{"x", Int32s({3, std::nullopt, -5, 10})},
                                                       {"tag", Strings({"c", "n", "a", "d"})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "-5,a\n3,c\n10,d\nnull,n\n");
  EXPECT_TRUE(out->column(0).IsNull(3));
  EXPECT_EQ(columnar::GetTypeName(out->schema().field(0).data_type), "Int32");
}

// DuckDB: select x from (values (1),(null),(2)) t(x) order by x desc  ->  2, 1, NULL
TEST(SortTest, DescendingAlsoPutsNullsLastLikeDuckDB) {
  SortOperator op{0, {SortKey{"x", false}}};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {MakeBatch({{"x", Int32s({1, std::nullopt, 2})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "2\n1\nnull\n");
}

TEST(SortTest, NumbersCompareNumerically) {
  SortOperator op{0, {SortKey{"d", true}}};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {MakeBatch({{"d", Doubles({10.0, 9.5, -2.25, 100.0})}}),
                                            MakeBatch({{"d", Doubles({0.0})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "-2.250000\n0.000000\n9.500000\n10.000000\n100.000000\n");
}

// DuckDB orders NaN above every number and before NULL: ASC -2, 1, nan, NULL;
// DESC nan, 1, -2, NULL.
TEST(SortTest, NaNSortsAboveNumbersAndBeforeNulls) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  auto input = [&] {
    return std::vector<RecordBatch>{
        MakeBatch({{"d", Doubles({1.0, nan, std::nullopt, -2.0, nan, 1.0, -2.0})}})};
  };
  SortOperator asc{0, {SortKey{"d", true}}};
  ASSERT_OK_AND_ASSIGN(auto asc_out, Drive(asc, input()));
  ASSERT_TRUE(asc_out.has_value());
  EXPECT_EQ(asc_out->ToCSV(), "-2.000000\n-2.000000\n1.000000\n1.000000\nnan\nnan\nnull\n");

  SortOperator desc{1, {SortKey{"d", false}}};
  ASSERT_OK_AND_ASSIGN(auto desc_out, Drive(desc, input()));
  ASSERT_TRUE(desc_out.has_value());
  EXPECT_EQ(desc_out->ToCSV(), "nan\nnan\n1.000000\n1.000000\n-2.000000\n-2.000000\nnull\n");
}

TEST(SortTest, StringsCompareBytewise) {
  SortOperator op{0, {SortKey{"s", true}}};
  ASSERT_OK_AND_ASSIGN(
      auto out, Drive(op, {MakeBatch({{"s", Strings({"b", "\xC3\xA9", "B", "a", "", "ab"})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "\nB\na\nab\nb\n\xC3\xA9\n");
}

TEST(SortTest, MultipleKeysWithMixedDirections) {
  SortOperator op{0, {SortKey{"mode", true}, SortKey{"n", false}}};
  ASSERT_OK_AND_ASSIGN(auto out,
                       Drive(op, {MakeBatch({{"mode", Strings({"SHIP", "AIR", "SHIP", "AIR"})},
                                             {"n", Int64s({1, 2, 3, std::nullopt})}}),
                                  MakeBatch({{"mode", Strings({"AIR"})}, {"n", Int64s({7})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "AIR,7\nAIR,2\nAIR,null\nSHIP,3\nSHIP,1\n");
}

TEST(SortTest, StableForEqualKeysAcrossBatches) {
  SortOperator op{0, {SortKey{"k", true}}};
  ASSERT_OK_AND_ASSIGN(auto out,
                       Drive(op, {MakeBatch({{"k", Int32s({2, 1, 2})}, {"seq", Int32s({0, 1, 2})}}),
                                  MakeBatch({{"k", Int32s({1, 2})}, {"seq", Int32s({3, 4})}}),
                                  MakeBatch({{"k", Int32s({std::nullopt, 1, std::nullopt})},
                                             {"seq", Int32s({5, 6, 7})}})}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), "1,1\n1,3\n1,6\n2,0\n2,2\n2,4\nnull,5\nnull,7\n");
}

TEST(SortTest, EmptyInputEmitsNothing) {
  SortOperator op{0, {SortKey{"x", true}}};
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {}));
  EXPECT_FALSE(out.has_value());

  SortOperator zero_rows{1, {SortKey{"x", true}}};
  ASSERT_OK_AND_ASSIGN(auto out2, Drive(zero_rows, {MakeBatch({{"x", Int32s({})}})}));
  EXPECT_FALSE(out2.has_value());
}

TEST(SortTest, MissingColumnIsSchemaError) {
  SortOperator op{0, {SortKey{"nope", true}}};
  ASSERT_OK(op.Sink(Block(MakeBatch({{"x", Int32s({1})}}))));
  auto status = op.FinalizeSink();
  ASSERT_FALSE(status.has_value());
  EXPECT_EQ(status.error().code(), ErrorCode::SchemaError);
}

// =============================================================================
// Pipeline-role contract
// =============================================================================

TEST(SortTest, RoleContract) {
  SortOperator op{0, {SortKey{"x", true}}};
  EXPECT_EQ(op.type(), OperatorType::Sort);
  EXPECT_TRUE(op.IsBlocking());
  EXPECT_TRUE(op.IsSink());
  EXPECT_FALSE(op.IsSource());
  EXPECT_FALSE(op.GetNextBatch().has_value());

  ASSERT_OK(op.Sink(Block(MakeBatch({{"x", Int32s({2, 1})}}))));
  EXPECT_FALSE(op.IsSource());

  ASSERT_OK(op.FinalizeSink());
  EXPECT_TRUE(op.IsSource());
  EXPECT_FALSE(op.FinalizeSink().has_value());
  EXPECT_FALSE(op.Sink(Block(MakeBatch({{"x", Int32s({3})}}))).has_value());

  ASSERT_OK_AND_ASSIGN(auto first, op.GetNextBatch());
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ((*first)->num_rows(), 2U);
  for (int i = 0; i < 2; ++i) {
    ASSERT_OK_AND_ASSIGN(auto eos, op.GetNextBatch());
    EXPECT_FALSE(eos.has_value());
  }
}

// =============================================================================
// Thread safety
// =============================================================================

/// @brief @p count batches of 50 rows; column "k" holds a permutation of 0..count*50-1.
[[nodiscard]] std::vector<RecordBatch> PermutedBatches(std::size_t count) {
  const std::size_t total = count * 50;
  std::vector<RecordBatch> batches;
  for (std::size_t b = 0; b < count; ++b) {
    std::vector<std::optional<int64_t>> keys;
    std::vector<std::optional<std::string>> payload;
    for (std::size_t r = 0; r < 50; ++r) {
      const auto k = static_cast<int64_t>(((b * 50 + r) * 7919) % total);
      keys.emplace_back(k);
      payload.emplace_back("p" + std::to_string(k));
    }
    batches.push_back(MakeBatch({{"k", Int64s(keys)}, {"p", Strings(payload)}}));
  }
  return batches;
}

[[nodiscard]] std::string ExpectedPermutedCsv(std::size_t count) {
  std::string csv;
  for (std::size_t k = 0; k < count * 50; ++k) {
    csv += std::to_string(k) + ",p" + std::to_string(k) + "\n";
  }
  return csv;
}

TEST(SortTest, ConcurrentSinkSortsAllRows) {
  constexpr std::size_t kBatches = 160;
  const auto batches = PermutedBatches(kBatches);

  SortOperator op{0, {SortKey{"k", true}}};
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
  ASSERT_OK_AND_ASSIGN(auto out, Drive(op, {}));
  ASSERT_TRUE(out.has_value());
  EXPECT_EQ(out->ToCSV(), ExpectedPermutedCsv(kBatches));
}

TEST(SortTest, PipelineExecutorWithWorkersSortsAllRows) {
  constexpr std::size_t kBatches = 200;
  BatchListSource source{0, PermutedBatches(kBatches)};
  SortOperator sort{1, {SortKey{"k", true}}};
  CollectorOperator collector{2};
  GraphBuilder gb;
  gb.Chain(source, sort, collector);
  ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());
  graph->Materialize();
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(*graph));
  ASSERT_EQ(pipelines.size(), 2U);

  PipelineExecutor executor{4};
  ASSERT_OK(executor.Execute(std::move(pipelines)));

  ASSERT_OK_AND_ASSIGN(auto collected, collector.CollectBatches());
  ASSERT_EQ(collected.size(), 1U);
  EXPECT_EQ(collected[0].ToCSV(), ExpectedPermutedCsv(kBatches));
}

}  // namespace
}  // namespace quest::taskflow
