// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_query_builder.cpp
/// @brief Unit tests for QueryBuilder DSL.

#include <gtest/gtest.h>

#include <filesystem>

#include "quest/builder/query_builder.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::datasource;

// =============================================================================
// ExprBuilder Tests
// =============================================================================

TEST(ExprBuilderTest, ColumnReference) {
  auto expr = Col("name");
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  EXPECT_NE(built->ToString().find("name"), std::string::npos);
}

TEST(ExprBuilderTest, LiteralInt) {
  auto expr = Lit(42);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  EXPECT_NE(built->ToString().find("42"), std::string::npos);
}

TEST(ExprBuilderTest, LiteralDouble) {
  auto expr = Lit(3.14);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
}

TEST(ExprBuilderTest, LiteralString) {
  auto expr = Lit(std::string("hello"));
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
}

TEST(ExprBuilderTest, LiteralBool) {
  auto expr_true = Lit(true);
  auto expr_false = Lit(false);

  EXPECT_NE(expr_true.Build(), nullptr);
  EXPECT_NE(expr_false.Build(), nullptr);
}

// =============================================================================
// Comparison Operators
// =============================================================================

TEST(ExprBuilderTest, Equality) {
  auto expr = Col("id") == Lit(10);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find("=="), std::string::npos);
}

TEST(ExprBuilderTest, NotEqual) {
  auto expr = Col("status") != Lit(0);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find("!="), std::string::npos);
}

TEST(ExprBuilderTest, LessThan) {
  auto expr = Col("age") < Lit(18);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find("<"), std::string::npos);
}

TEST(ExprBuilderTest, LessOrEqual) {
  auto expr = Col("score") <= Lit(100);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find("<="), std::string::npos);
}

TEST(ExprBuilderTest, GreaterThan) {
  auto expr = Col("salary") > Lit(50000);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find(">"), std::string::npos);
}

TEST(ExprBuilderTest, GreaterOrEqual) {
  auto expr = Col("level") >= Lit(5);
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find(">="), std::string::npos);
}

// =============================================================================
// Logical Operators
// =============================================================================

TEST(ExprBuilderTest, LogicalAnd) {
  auto expr = (Col("age") >= Lit(18)) && (Col("age") < Lit(65));
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find("AND"), std::string::npos);
}

TEST(ExprBuilderTest, LogicalOr) {
  auto expr = (Col("status") == Lit(1)) || (Col("status") == Lit(2));
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
  auto str = built->ToString();
  EXPECT_NE(str.find("OR"), std::string::npos);
}

TEST(ExprBuilderTest, ComplexLogical) {
  auto expr = ((Col("a") > Lit(0)) && (Col("b") < Lit(100))) ||
              (Col("c") == Lit(0));
  auto built = expr.Build();

  EXPECT_NE(built, nullptr);
}

// =============================================================================
// QueryBuilder Tests with Mock DataSource
// =============================================================================

class MockDataSource : public DataSource {
 public:
  explicit MockDataSource(Schema schema) : schema_(std::move(schema)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] RecordBatchSequence Scan(
      const std::vector<std::string>& /*projection*/) override {
    // Return empty sequence for testing
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

TEST_F(QueryBuilderTest, BuildPhysicalPlan) {
  auto result = QueryBuilder::From(data_source_, "test")
                    .Select({"id", "name"})
                    .BuildPhysicalPlan();

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

TEST_F(QueryBuilderParquetTest, ExecuteCollect) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto builder_result = QueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  auto batches = std::move(builder_result.value()).ExecuteCollect();

  EXPECT_TRUE(batches.has_value());
  EXPECT_GT(batches.value().size(), 0U);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST_F(QueryBuilderTest, SelectAll) {
  // Select all columns
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
