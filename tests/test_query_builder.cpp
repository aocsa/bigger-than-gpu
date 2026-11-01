// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_query_builder.cpp
/// @brief Unit tests for QueryBuilder (synchronous execution).

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>

#include "quest/builder/query_builder.hpp"
#include "quest/datasource/data_source.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::columnar;
using namespace quest::datasource;

// =============================================================================
// QueryBuilder Tests with Mock DataSource
// =============================================================================

class MockDataSource : public DataSource {
 public:
  explicit MockDataSource(Schema schema) : schema_(std::move(schema)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] size_t num_partitions() const override { return 0; }

  [[nodiscard]] Result<RecordBatch> ReadPartition(
      size_t /*index*/, const std::vector<std::string>& /*projection*/) override {
    return Error::OutOfRange("MockDataSource has no partitions");
  }

 private:
  Schema schema_;
};

class QueryBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::vector<Field> fields = {
        Field("id", DataType::kInt32),
        Field("name", DataType::kString),
        Field("salary", DataType::kDouble),
        Field("department", DataType::kString),
    };
    schema_ = Schema(fields);
    data_source_ = std::make_shared<MockDataSource>(schema_);
  }

  Schema schema_;
  std::shared_ptr<DataSource> data_source_;
};

TEST_F(QueryBuilderTest, FromDataSource) {
  auto builder = QueryBuilder::From(data_source_, "test");
  EXPECT_FALSE(builder.ToString().empty());
}

TEST_F(QueryBuilderTest, SelectColumns) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Select({"id", "name"})
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
  auto str = plan->ToString();
  EXPECT_NE(str.find("Projection"), std::string::npos);
}

TEST_F(QueryBuilderTest, WhereClause) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Where(Col("salary") > Lit(50000.0))
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
  auto str = plan->ToString();
  EXPECT_NE(str.find("Selection"), std::string::npos);
}

TEST_F(QueryBuilderTest, SelectAndWhere) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Where(Col("department") == Lit(std::string("Engineering")))
                  .Select({"id", "name", "salary"})
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
}

TEST_F(QueryBuilderTest, LowersToGraph) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Select({"id", "name"})
                  .BuildLogicalPlan();
  auto result = execution::CreateGraph(*plan);

  EXPECT_TRUE(result.has_value());
  EXPECT_NE(result.value(), nullptr);
}

TEST_F(QueryBuilderTest, ToString) {
  auto builder = QueryBuilder::From(data_source_, "test");
  auto str = builder.ToString();

  EXPECT_FALSE(str.empty());
  EXPECT_NE(str.find("Scan"), std::string::npos);
}

// =============================================================================
// QueryBuilder with Parquet Files
// =============================================================================

class QueryBuilderParquetTest : public ::testing::Test {
 protected:
  std::filesystem::path test_data_dir_ =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "examples";

  std::filesystem::path employees_file() const {
    return test_data_dir_ / "employees_uncompressed.parquet";
  }
};

TEST_F(QueryBuilderParquetTest, FromParquet) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto result = QueryBuilder::FromParquet(employees_file().string());
  EXPECT_TRUE(result.has_value());
}

TEST_F(QueryBuilderParquetTest, FromParquetInvalid) {
  auto result = QueryBuilder::FromParquet("/nonexistent/file.parquet");
  EXPECT_FALSE(result.has_value());
}

TEST_F(QueryBuilderParquetTest, ExecuteOnGraph) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto builder_result = QueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  auto batches = std::move(builder_result.value()).ExecuteOnGraph();

  EXPECT_TRUE(batches.has_value());
  EXPECT_GT(batches.value().size(), 0U);
}

TEST(QueryBuilderDateTest, ReceiptDateRangeMatchesDuckDb) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" /
                    "tpch" / "lineitem.parquet";
  using std::chrono::year;
  ASSERT_OK_AND_ASSIGN(auto builder, QueryBuilder::FromParquet(path.string()));
  ASSERT_OK_AND_ASSIGN(auto batches,
                       std::move(builder)
                           .Where(Col("l_receiptdate") >= LitDate(year{1994} / 1 / 1) &&
                                  Col("l_receiptdate") < LitDate(year{1995} / 1 / 1))
                           .ExecuteOnGraph());

  // duckdb: select count(*) from 'testdata/tpch/lineitem.parquet'
  //   where l_receiptdate >= date '1994-01-01' and l_receiptdate < date '1995-01-01'
  size_t rows = 0;
  for (const auto& batch : batches) {
    rows += batch.num_rows();
    const auto* receipt = batch.GetColumnByName("l_receiptdate");
    ASSERT_NE(receipt, nullptr);
    for (size_t i = 0; i < batch.num_rows(); ++i) {
      const Value v = receipt->GetValue(i);
      const auto* d = std::get_if<Date32>(&v);
      ASSERT_NE(d, nullptr);
      EXPECT_GE(d->days, 8766);
      EXPECT_LT(d->days, 9131);
    }
  }
  EXPECT_EQ(rows, 9525U);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(QueryBuilderTest, SelectAll) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Select({"id", "name", "department", "salary"})
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
}

TEST_F(QueryBuilderTest, MultipleWheres) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Where(Col("salary") > Lit(30000.0))
                  .Where(Col("salary") < Lit(100000.0))
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
}

TEST_F(QueryBuilderTest, ChainedOperations) {
  auto plan = QueryBuilder::From(data_source_, "test")
                  .Where(Col("id") > Lit(0))
                  .Select({"id", "name"})
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
  auto str = plan->Pretty(0);
  EXPECT_FALSE(str.empty());
}

// =============================================================================
// QueryBuilder Move Semantics Tests
// =============================================================================

TEST_F(QueryBuilderTest, MoveConstruction) {
  auto builder1 = QueryBuilder::From(data_source_, "test");
  auto str1 = builder1.ToString();

  QueryBuilder builder2(std::move(builder1));

  EXPECT_EQ(builder2.ToString(), str1);
}

TEST_F(QueryBuilderTest, SchemaAccess) {
  auto builder = QueryBuilder::From(data_source_, "test");
  auto schema = builder.schema();

  EXPECT_EQ(schema.num_fields(), 4);
  EXPECT_EQ(schema.field(0).name, "id");
}

TEST_F(QueryBuilderTest, BoolConversion) {
  auto builder = QueryBuilder::From(data_source_, "test");
  EXPECT_TRUE(static_cast<bool>(builder));
}
