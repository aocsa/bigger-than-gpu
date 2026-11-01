// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_tpch.cpp
/// @brief TPC-H Q1, Q3, Q5, Q6 and Q10 end to end on SF 0.01 (testdata/tpch), each checked
/// against DuckDB's answer in testdata/tpch/answers (written by testdata/tpch/generate.sql).

#include <gtest/gtest.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <ranges>
#include <string>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace std::chrono_literals;
using plan::logical::SortKey;

namespace {

using Rows = std::vector<std::vector<std::string>>;

[[nodiscard]] std::filesystem::path TpchDir() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "tpch";
}

[[nodiscard]] QueryBuilder Scan(const std::string& table) {
  return QueryBuilder::FromParquet((TpchDir() / (table + ".parquet")).string()).value();
}

/// @brief `l_extendedprice * (1 - l_discount)`, the revenue of a line.
[[nodiscard]] ExprBuilder DiscountedPrice() {
  return Col("l_extendedprice") * (Lit(1) - Col("l_discount"));
}

/// @brief The '|'-separated rows of answers/@p name.tbl.
[[nodiscard]] Rows ReadAnswer(const std::string& name) {
  std::ifstream in(TpchDir() / "answers" / (name + ".tbl"));
  Rows rows;
  for (std::string line; std::getline(in, line);) {
    auto& row = rows.emplace_back();
    for (auto cell : std::views::split(line, '|')) {
      row.emplace_back(cell.begin(), cell.end());
    }
  }
  return rows;
}

/// @brief A result cell as text, and whether it is a Double (only those match approximately).
struct Cell {
  std::string text;
  bool is_double = false;
};

[[nodiscard]] std::vector<std::vector<Cell>> ToRows(const std::vector<columnar::RecordBatch>& batches) {
  std::vector<std::vector<Cell>> rows;
  for (const auto& batch : batches) {
    for (std::size_t r = 0; r < batch.num_rows(); ++r) {
      auto& row = rows.emplace_back();
      for (std::size_t c = 0; c < batch.num_columns(); ++c) {
        const auto& column = batch.column(c);
        // FormatCell prints doubles with 6 decimals, too few to compare an AVG.
        if (column.GetType() == columnar::DataType::kDouble && !column.IsNull(r)) {
          row.push_back(Cell{std::format("{}", column.Get<double>(r)), true});
        } else {
          row.push_back(Cell{columnar::FormatCell(column, r)});
        }
      }
    }
  }
  return rows;
}

[[nodiscard]] std::optional<double> ParseNumber(const std::string& s) {
  double value = 0;
  const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
  if (ec != std::errc{} || end != s.data() + s.size()) {
    return std::nullopt;
  }
  return value;
}

/// @brief Every cell matches DuckDB's text exactly (Decimal money included), except a Double
/// (an AVG), which matches within a relative 1e-9.
void ExpectRowsMatch(const std::vector<std::vector<Cell>>& actual, const Rows& expected) {
  ASSERT_EQ(actual.size(), expected.size());
  for (std::size_t r = 0; r < expected.size(); ++r) {
    ASSERT_EQ(actual[r].size(), expected[r].size()) << "row " << r;
    for (std::size_t c = 0; c < expected[r].size(); ++c) {
      const Cell& cell = actual[r][c];
      const auto want = ParseNumber(expected[r][c]);
      const auto got = ParseNumber(cell.text);
      if (cell.is_double && want && got) {
        EXPECT_LE(std::abs(*got - *want), 1e-9 * std::max(1.0, std::abs(*want)))
            << "row " << r << " column " << c << ": " << cell.text << " vs " << expected[r][c];
      } else {
        EXPECT_EQ(cell.text, expected[r][c]) << "row " << r << " column " << c;
      }
    }
  }
}

void ExpectMatchesDuckDb(QueryBuilder query, const std::string& answer) {
  ASSERT_OK_AND_ASSIGN(auto batches, std::move(query).ExecuteOnGraph());
  ExpectRowsMatch(ToRows(batches), ReadAnswer(answer));
}

}  // namespace

TEST(TpchTest, Q1PricingSummary) {
  const auto price = Col("l_extendedprice");
  auto query = Scan("lineitem")
                   .Where(Col("l_shipdate") <= LitDate(1998y / 9 / 2))
                   .GroupBy({"l_returnflag", "l_linestatus"})
                   .Aggregate({{"sum_qty", agg::Sum(Col("l_quantity"))},
                               {"sum_base_price", agg::Sum(price)},
                               {"sum_disc_price", agg::Sum(DiscountedPrice())},
                               {"sum_charge", agg::Sum(DiscountedPrice() * (Lit(1) + Col("l_tax")))},
                               {"avg_qty", agg::Avg(Col("l_quantity"))},
                               {"avg_price", agg::Avg(price)},
                               {"avg_disc", agg::Avg(Col("l_discount"))},
                               {"count_order", agg::CountStar()}})
                   .Select({"l_returnflag", "l_linestatus", "sum_qty", "sum_base_price",
                            "sum_disc_price", "sum_charge", "avg_qty", "avg_price", "avg_disc",
                            "count_order"})
                   .OrderBy({"l_returnflag", "l_linestatus"});
  ExpectMatchesDuckDb(std::move(query), "q1");
}

TEST(TpchTest, Q3ShippingPriority) {
  auto customer = Scan("customer").Where(Col("c_mktsegment") == Lit("BUILDING"));
  auto orders = Scan("orders").Where(Col("o_orderdate") < LitDate(1995y / 3 / 15));
  auto query = Scan("lineitem")
                   .Where(Col("l_shipdate") > LitDate(1995y / 3 / 15))
                   .Join(std::move(orders), "l_orderkey", "o_orderkey")
                   .Join(std::move(customer), "o_custkey", "c_custkey")
                   .GroupBy({"l_orderkey", "o_orderdate", "o_shippriority"})
                   .Aggregate({{"revenue", agg::Sum(DiscountedPrice())}})
                   .Select({"l_orderkey", "revenue", "o_orderdate", "o_shippriority"})
                   .OrderBy(std::vector<SortKey>{{"revenue", false}, {"o_orderdate", true}})
                   .Limit(10);
  ExpectMatchesDuckDb(std::move(query), "q3");
}

TEST(TpchTest, Q5LocalSupplierVolume) {
  auto orders = Scan("orders").Where(Col("o_orderdate") >= LitDate(1994y / 1 / 1) &&
                                     Col("o_orderdate") < LitDate(1995y / 1 / 1));
  auto region = Scan("region").Where(Col("r_name") == Lit("ASIA"));
  // c_nationkey = s_nationkey is the second key of the supplier join, applied as a filter.
  auto query = Scan("lineitem")
                   .Join(std::move(orders), "l_orderkey", "o_orderkey")
                   .Join(Scan("customer"), "o_custkey", "c_custkey")
                   .Join(Scan("supplier"), "l_suppkey", "s_suppkey")
                   .Where(Col("c_nationkey") == Col("s_nationkey"))
                   .Join(Scan("nation"), "s_nationkey", "n_nationkey")
                   .Join(std::move(region), "n_regionkey", "r_regionkey")
                   .GroupBy({"n_name"})
                   .Aggregate({{"revenue", agg::Sum(DiscountedPrice())}})
                   .Select({"n_name", "revenue"})
                   .OrderBy(std::vector<SortKey>{{"revenue", false}});
  ExpectMatchesDuckDb(std::move(query), "q5");
}

TEST(TpchTest, Q6ForecastingRevenueChange) {
  auto query = Scan("lineitem")
                   .Where(Col("l_shipdate") >= LitDate(1994y / 1 / 1) &&
                          Col("l_shipdate") < LitDate(1995y / 1 / 1) &&
                          Col("l_discount") >= LitDecimal<5, 2>() &&
                          Col("l_discount") <= LitDecimal<7, 2>() && Col("l_quantity") < Lit(24))
                   .GroupBy({})
                   .Aggregate({{"revenue", agg::Sum(Col("l_extendedprice") * Col("l_discount"))}});
  ExpectMatchesDuckDb(std::move(query), "q6");
}

TEST(TpchTest, Q10ReturnedItemReporting) {
  auto orders = Scan("orders").Where(Col("o_orderdate") >= LitDate(1993y / 10 / 1) &&
                                     Col("o_orderdate") < LitDate(1994y / 1 / 1));
  auto query = Scan("lineitem")
                   .Where(Col("l_returnflag") == Lit("R"))
                   .Join(std::move(orders), "l_orderkey", "o_orderkey")
                   .Join(Scan("customer"), "o_custkey", "c_custkey")
                   .Join(Scan("nation"), "c_nationkey", "n_nationkey")
                   .GroupBy({"c_custkey", "c_name", "c_acctbal", "c_phone", "n_name", "c_address",
                             "c_comment"})
                   .Aggregate({{"revenue", agg::Sum(DiscountedPrice())}})
                   .Select({"c_custkey", "c_name", "revenue", "c_acctbal", "n_name", "c_address",
                            "c_phone", "c_comment"})
                   .OrderBy(std::vector<SortKey>{{"revenue", false}})
                   .Limit(20);
  ExpectMatchesDuckDb(std::move(query), "q10");
}

TEST(TpchTest, SumOverNoRowsKeepsTheDecimalType) {
  auto query = Scan("lineitem")
                   .Where(Col("l_quantity") < Lit(0))
                   .GroupBy({})
                   .Aggregate({{"revenue", agg::Sum(Col("l_extendedprice") * Col("l_discount"))}});
  ASSERT_OK_AND_ASSIGN(auto batches, std::move(query).ExecuteOnGraph());
  ASSERT_EQ(batches.size(), 1U);
  ASSERT_EQ(batches[0].num_rows(), 1U);
  EXPECT_EQ(batches[0].column(0).GetType(), columnar::DataType::Decimal(18, 4));
  EXPECT_TRUE(batches[0].column(0).IsNull(0));
}
