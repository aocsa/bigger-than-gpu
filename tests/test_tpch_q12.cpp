// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_tpch_q12.cpp
/// @brief TPC-H Q12 end to end on SF 0.01 (testdata/tpch), checked against DuckDB.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <tuple>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/execution/lowering/plan_to_graph.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace std::chrono_literals;

namespace {

[[nodiscard]] std::filesystem::path TpchDir() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "tpch";
}

// SELECT l_shipmode,
//   SUM(CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END),
//   SUM(CASE WHEN o_orderpriority <> '1-URGENT' AND o_orderpriority <> '2-HIGH' THEN 1 ELSE 0 END)
// FROM orders, lineitem
// WHERE o_orderkey = l_orderkey AND l_shipmode IN ('MAIL', 'SHIP')
//   AND l_commitdate < l_receiptdate AND l_shipdate < l_commitdate
//   AND l_receiptdate >= DATE '1994-01-01' AND l_receiptdate < DATE '1995-01-01'
// GROUP BY l_shipmode ORDER BY l_shipmode
[[nodiscard]] QueryBuilder Q12() {
  auto lineitem =
      QueryBuilder::FromParquet((TpchDir() / "lineitem.parquet").string())
          .value()
          .Where((Col("l_shipmode") == Lit("MAIL") || Col("l_shipmode") == Lit("SHIP")) &&
                 Col("l_commitdate") < Col("l_receiptdate") &&
                 Col("l_shipdate") < Col("l_commitdate") &&
                 Col("l_receiptdate") >= LitDate(1994y / 1 / 1) &&
                 Col("l_receiptdate") < LitDate(1995y / 1 / 1));

  auto urgent = Col("o_orderpriority") == Lit("1-URGENT") ||
                Col("o_orderpriority") == Lit("2-HIGH");
  auto not_urgent = Col("o_orderpriority") != Lit("1-URGENT") &&
                    Col("o_orderpriority") != Lit("2-HIGH");

  return QueryBuilder::FromParquet((TpchDir() / "orders.parquet").string())
      .value()
      .Join(std::move(lineitem), "o_orderkey", "l_orderkey")
      .GroupBy({"l_shipmode"})
      .Aggregate({{"high_line_count", agg::Sum(Case(urgent, Lit(1), Lit(0)))},
                  {"low_line_count", agg::Sum(Case(not_urgent, Lit(1), Lit(0)))}})
      .Select({"l_shipmode", "high_line_count", "low_line_count"})
      .OrderBy({"l_shipmode"});
}

[[nodiscard]] std::vector<OperatorType> Types(const Pipeline& pipeline) {
  std::vector<OperatorType> types;
  for (const auto* op : pipeline.operators()) {
    types.push_back(op->type());
  }
  return types;
}

}  // namespace

TEST(TpchQ12Test, PlanSplitsIntoTheFourExpectedPipelines) {
  auto plan = Q12().BuildLogicalPlan();
  ASSERT_OK_AND_ASSIGN(auto exec, execution::CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  ASSERT_EQ(pipelines.size(), 4U);
  const auto& p0 = *pipelines[0];
  const auto& p1 = *pipelines[1];
  const auto& p2 = *pipelines[2];
  const auto& p3 = *pipelines[3];
  // P0 table_scan lineitem -> filter -> hash_join(build)
  EXPECT_EQ(Types(p0), (std::vector{OperatorType::TableScan, OperatorType::Filter,
                                    OperatorType::HashJoin}));
  // P1 table_scan orders -> hash_join(probe) -> grouped_aggregate
  EXPECT_EQ(Types(p1), (std::vector{OperatorType::TableScan, OperatorType::HashJoin,
                                    OperatorType::HashAggregate}));
  // P2 projection -> order
  EXPECT_EQ(Types(p2), (std::vector{OperatorType::HashAggregate, OperatorType::Project,
                                    OperatorType::Sort}));
  // P3 result_collector
  EXPECT_EQ(Types(p3).back(), OperatorType::Output);

  EXPECT_EQ(p1.FullBarrierProducers(), std::vector<Pipeline*>{pipelines[0].get()});
  EXPECT_EQ(p2.FullBarrierProducers(), std::vector<Pipeline*>{pipelines[1].get()});
  EXPECT_EQ(p3.FullBarrierProducers(), std::vector<Pipeline*>{pipelines[2].get()});
}

TEST(TpchQ12Test, MatchesDuckDb) {
  ASSERT_OK_AND_ASSIGN(auto batches, Q12().ExecuteOnGraph());

  std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> rows;
  for (const auto& batch : batches) {
    ASSERT_EQ(batch.num_columns(), 3U);
    EXPECT_EQ(batch.schema().field(0).name, "l_shipmode");
    EXPECT_EQ(batch.schema().field(1).name, "high_line_count");
    EXPECT_EQ(batch.schema().field(2).name, "low_line_count");
    for (std::size_t i = 0; i < batch.num_rows(); ++i) {
      rows.emplace_back(std::get<std::string>(batch.column(0).GetValue(i)),
                        std::get<std::int64_t>(batch.column(1).GetValue(i)),
                        std::get<std::int64_t>(batch.column(2).GetValue(i)));
    }
  }

  // DuckDB on testdata/tpch (see testdata/tpch/generate.sql).
  const std::vector<std::tuple<std::string, std::int64_t, std::int64_t>> expected = {
      {"MAIL", 64, 86},
      {"SHIP", 61, 96},
  };
  EXPECT_EQ(rows, expected);
}

TEST(TpchQ12Test, DateColumnsGroupMinMaxAndSortLikeDuckDb) {
  ASSERT_OK_AND_ASSIGN(
      auto batches,
      QueryBuilder::FromParquet((TpchDir() / "lineitem.parquet").string())
          .value()
          .GroupBy({"l_shipmode"})
          .Aggregate({{"first_ship", agg::Min(Col("l_shipdate"))},
                      {"last_receipt", agg::Max(Col("l_receiptdate"))},
                      {"n", agg::CountStar()}})
          .OrderBy({"l_shipmode"})
          .ExecuteOnGraph());

  std::vector<std::string> rows;
  for (const auto& batch : batches) {
    for (std::size_t i = 0; i < batch.num_rows(); ++i) {
      rows.push_back(columnar::ValueToString(batch.column(0).GetValue(i)) + " " +
                     columnar::ValueToString(batch.column(1).GetValue(i)) + " " +
                     columnar::ValueToString(batch.column(2).GetValue(i)) + " " +
                     columnar::ValueToString(batch.column(3).GetValue(i)));
    }
  }

  // DuckDB: SELECT l_shipmode, min(l_shipdate), max(l_receiptdate), count(*)
  //         FROM lineitem GROUP BY l_shipmode ORDER BY l_shipmode
  const std::vector<std::string> expected = {
      "AIR 1992-01-11 1998-12-25 8491",     "FOB 1992-01-13 1998-12-18 8641",
      "MAIL 1992-01-06 1998-12-17 8669",    "RAIL 1992-01-04 1998-12-15 8566",
      "REG AIR 1992-01-06 1998-12-21 8616", "SHIP 1992-01-19 1998-12-12 8482",
      "TRUCK 1992-01-09 1998-12-24 8710",
  };
  EXPECT_EQ(rows, expected);
}
