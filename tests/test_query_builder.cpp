// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_query_builder.cpp
/// @brief Unit tests for QueryBuilder (synchronous execution).

#include <gtest/gtest.h>

#include <filesystem>

#include "quest/builder/query_builder.hpp"
#include "quest/datasource/data_source.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::datasource;

// =============================================================================
// QueryBuilder Tests with Mock DataSource
// =============================================================================

class MockDataSource : public DataSource {
 public:
  explicit MockDataSource(Schema schema) : schema_(std::move(schema)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] RecordBatchSequence Scan(
      const std::vector<std::string>& /*projection*/) override {
    return RecordBatchSequence(nullptr, {});
  }

 private:
  Schema schema_;
};

class QueryBuilderTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::vector<Field> fields = {
        Field("id", Int32Type{}),
        Field("name", StringType{}),
        Field("salary", DoubleType{}),
        Field("department", StringType{}),
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
