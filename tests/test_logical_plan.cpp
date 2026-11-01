// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_logical_plan.cpp
/// @brief Unit tests for LogicalPlan and LogicalExpr classes.

#include <gtest/gtest.h>

#include "quest/plan/logical/logical_expr.hpp"
#include "quest/plan/logical/logical_plan.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::columnar;
using namespace quest::datasource;
using namespace quest::plan::logical;

// =============================================================================
// Mock DataSource for Testing
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

// =============================================================================
// LogicalExpr Tests - Column References
// =============================================================================

TEST(LogicalExprTest, ColumnByName) {
  auto expr = col("my_column");

  EXPECT_NE(expr, nullptr);
  EXPECT_EQ(expr->ToString(), "#my_column");

  auto* col_expr = dynamic_cast<Column*>(expr.get());
  ASSERT_NE(col_expr, nullptr);
  EXPECT_EQ(col_expr->name(), "my_column");
}

TEST(LogicalExprTest, ColumnByIndex) {
  auto expr = col_idx(5);

  EXPECT_NE(expr, nullptr);
  EXPECT_EQ(expr->ToString(), "#5");

  auto* idx_expr = dynamic_cast<ColumnIndex*>(expr.get());
  ASSERT_NE(idx_expr, nullptr);
  EXPECT_EQ(idx_expr->index(), 5U);
}

// =============================================================================
// LogicalExpr Tests - Literals
// =============================================================================

TEST(LogicalExprTest, LiteralInt32) {
  auto expr = lit(42);
  EXPECT_EQ(expr->ToString(), "42");
}

TEST(LogicalExprTest, LiteralInt64) {
  auto expr = lit(int64_t{1000000000000});
  EXPECT_NE(expr->ToString().find("1000000000000"), std::string::npos);
}

TEST(LogicalExprTest, LiteralDouble) {
  auto expr = lit(3.14);
  EXPECT_NE(expr->ToString().find("3.14"), std::string::npos);
}

TEST(LogicalExprTest, LiteralBool) {
  auto expr_true = lit(true);
  auto expr_false = lit(false);

  EXPECT_EQ(expr_true->ToString(), "true");
  EXPECT_EQ(expr_false->ToString(), "false");
}

TEST(LogicalExprTest, LiteralString) {
  auto expr = lit(std::string("hello world"));
  EXPECT_NE(expr->ToString().find("hello world"), std::string::npos);
}

// =============================================================================
// LogicalExpr Tests - Comparison Operators
// =============================================================================

TEST(LogicalExprTest, Equality) {
  auto expr = eq(col("a"), lit(10));
  auto str = expr->ToString();

  EXPECT_NE(str.find("=="), std::string::npos);
  EXPECT_NE(str.find("#a"), std::string::npos);
  EXPECT_NE(str.find("10"), std::string::npos);
}

TEST(LogicalExprTest, NotEqual) {
  auto expr = neq(col("b"), lit(0));
  EXPECT_NE(expr->ToString().find("!="), std::string::npos);
}

TEST(LogicalExprTest, GreaterThan) {
  auto expr = gt(col("x"), col("y"));
  EXPECT_NE(expr->ToString().find(">"), std::string::npos);
}

TEST(LogicalExprTest, GreaterOrEqual) {
  auto expr = gte(col("score"), lit(60));
  EXPECT_NE(expr->ToString().find(">="), std::string::npos);
}

TEST(LogicalExprTest, LessThan) {
  auto expr = lt(col("age"), lit(18));
  EXPECT_NE(expr->ToString().find("<"), std::string::npos);
}

TEST(LogicalExprTest, LessOrEqual) {
  auto expr = lte(col("price"), lit(100.0));
  EXPECT_NE(expr->ToString().find("<="), std::string::npos);
}

// =============================================================================
// LogicalExpr Tests - Arithmetic Operators
// =============================================================================

TEST(LogicalExprTest, Addition) {
  auto expr = add(col("a"), col("b"));
  EXPECT_NE(expr->ToString().find("+"), std::string::npos);
}

TEST(LogicalExprTest, Subtraction) {
  auto expr = subtract(col("total"), col("discount"));
  EXPECT_NE(expr->ToString().find("-"), std::string::npos);
}

TEST(LogicalExprTest, Multiplication) {
  auto expr = multiply(col("price"), col("quantity"));
  EXPECT_NE(expr->ToString().find("*"), std::string::npos);
}

TEST(LogicalExprTest, Division) {
  auto expr = divide(col("sum"), col("count"));
  EXPECT_NE(expr->ToString().find("/"), std::string::npos);
}

TEST(LogicalExprTest, Modulo) {
  auto expr = modulo(col("x"), lit(10));
  EXPECT_NE(expr->ToString().find("%"), std::string::npos);
}

// =============================================================================
// LogicalExpr Tests - Logical Operators
// =============================================================================

TEST(LogicalExprTest, LogicalAnd) {
  auto expr = and_expr(gt(col("a"), lit(0)), lt(col("a"), lit(100)));
  EXPECT_NE(expr->ToString().find("AND"), std::string::npos);
}

TEST(LogicalExprTest, LogicalOr) {
  auto expr = or_expr(eq(col("status"), lit(1)), eq(col("status"), lit(2)));
  EXPECT_NE(expr->ToString().find("OR"), std::string::npos);
}

// =============================================================================
// LogicalExpr Tests - Aggregate Functions
// =============================================================================

TEST(LogicalExprTest, Sum) {
  auto expr = sum(col("salary"));
  EXPECT_EQ(expr->ToString(), "SUM(#salary)");
}

TEST(LogicalExprTest, Avg) {
  auto expr = avg(col("score"));
  EXPECT_EQ(expr->ToString(), "AVG(#score)");
}

TEST(LogicalExprTest, Count) {
  auto expr = count(col("id"));
  EXPECT_EQ(expr->ToString(), "COUNT(#id)");
}

TEST(LogicalExprTest, CountStar) {
  auto expr = count_star();
  EXPECT_EQ(expr->ToString(), "COUNT(*)");
}

TEST(LogicalExprTest, Min) {
  auto expr = min(col("price"));
  EXPECT_EQ(expr->ToString(), "MIN(#price)");
}

TEST(LogicalExprTest, Max) {
  auto expr = max(col("price"));
  EXPECT_EQ(expr->ToString(), "MAX(#price)");
}

// =============================================================================
// LogicalExpr Tests - Clone
// =============================================================================

TEST(LogicalExprTest, CloneColumn) {
  auto expr = col("test");
  auto cloned = expr->Clone();

  EXPECT_EQ(expr->ToString(), cloned->ToString());
  EXPECT_NE(expr.get(), cloned.get());
}

TEST(LogicalExprTest, CloneLiteral) {
  auto expr = lit(42);
  auto cloned = expr->Clone();

  EXPECT_EQ(expr->ToString(), cloned->ToString());
}

TEST(LogicalExprTest, CloneBinaryExpr) {
  auto expr = add(col("a"), col("b"));
  auto cloned = expr->Clone();

  EXPECT_EQ(expr->ToString(), cloned->ToString());
}

TEST(LogicalExprTest, CloneAggregate) {
  auto expr = sum(col("x"));
  auto cloned = expr->Clone();

  EXPECT_EQ(expr->ToString(), cloned->ToString());
}

// =============================================================================
// LogicalPlan Tests
// =============================================================================

class LogicalPlanTest : public ::testing::Test {
 protected:
  void SetUp() override {
    std::vector<Field> fields = {
        Field("id", Int32Type{}),
        Field("name", StringType{}),
        Field("value", DoubleType{}),
    };
    schema_ = Schema(fields);
    data_source_ = std::make_shared<MockDataSource>(schema_);
  }

  Schema schema_;
  std::shared_ptr<DataSource> data_source_;
};

TEST_F(LogicalPlanTest, ScanPlan) {
  Scan scan("test.parquet", data_source_, std::nullopt);

  EXPECT_EQ(scan.path(), "test.parquet");
  EXPECT_EQ(scan.schema().num_fields(), 3U);
  EXPECT_TRUE(scan.Children().empty());
}

TEST_F(LogicalPlanTest, ScanWithProjection) {
  std::vector<std::string> projection = {"id", "name"};
  Scan scan("test.parquet", data_source_, projection);

  EXPECT_EQ(scan.schema().num_fields(), 2U);
  EXPECT_EQ(scan.schema().field(0).name, "id");
  EXPECT_EQ(scan.schema().field(1).name, "name");
}

TEST_F(LogicalPlanTest, SelectionPlan) {
  auto scan = std::make_unique<Scan>("test.parquet", data_source_, std::nullopt);
  auto condition = gt(col("value"), lit(0.0));

  Selection selection(std::move(scan), std::move(condition));

  EXPECT_EQ(selection.schema().num_fields(), 3U);
  EXPECT_EQ(selection.Children().size(), 1U);
}

TEST_F(LogicalPlanTest, ProjectionPlan) {
  auto scan = std::make_unique<Scan>("test.parquet", data_source_, std::nullopt);

  std::vector<LogicalExprPtr> exprs;
  exprs.push_back(col("id"));
  exprs.push_back(col("name"));

  Projection projection(std::move(scan), std::move(exprs));

  EXPECT_EQ(projection.Children().size(), 1U);
}

TEST_F(LogicalPlanTest, PlanToString) {
  auto scan = std::make_unique<Scan>("test.parquet", data_source_, std::nullopt);
  auto str = scan->ToString();

  EXPECT_FALSE(str.empty());
  EXPECT_NE(str.find("Scan"), std::string::npos);
}

TEST_F(LogicalPlanTest, PlanPrettyPrint) {
  auto scan = std::make_unique<Scan>("test.parquet", data_source_, std::nullopt);
  auto condition = eq(col("id"), lit(1));
  auto selection =
      std::make_unique<Selection>(std::move(scan), std::move(condition));

  auto pretty = selection->Pretty(0);

  EXPECT_FALSE(pretty.empty());
}

// =============================================================================
// Complex Expression Tests
// =============================================================================

TEST(ComplexExprTest, NestedArithmetic) {
  // (a + b) * (c - d)
  auto expr = multiply(add(col("a"), col("b")), subtract(col("c"), col("d")));

  auto str = expr->ToString();
  EXPECT_NE(str.find("+"), std::string::npos);
  EXPECT_NE(str.find("*"), std::string::npos);
  EXPECT_NE(str.find("-"), std::string::npos);
}

TEST(ComplexExprTest, ComparisonWithArithmetic) {
  // (a + b) > 100
  auto expr = gt(add(col("a"), col("b")), lit(100));

  auto str = expr->ToString();
  EXPECT_NE(str.find("+"), std::string::npos);
  EXPECT_NE(str.find(">"), std::string::npos);
}

TEST(ComplexExprTest, ComplexCondition) {
  // (a > 0 AND b < 100) OR c == 0
  auto expr =
      or_expr(and_expr(gt(col("a"), lit(0)), lt(col("b"), lit(100))),
              eq(col("c"), lit(0)));

  auto str = expr->ToString();
  EXPECT_NE(str.find("AND"), std::string::npos);
  EXPECT_NE(str.find("OR"), std::string::npos);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(LogicalExprEdgeCaseTest, EmptyColumnName) {
  auto expr = col("");
  EXPECT_EQ(expr->ToString(), "#");
}

TEST(LogicalExprEdgeCaseTest, ZeroIndex) {
  auto expr = col_idx(0);
  EXPECT_EQ(expr->ToString(), "#0");
}

TEST(LogicalExprEdgeCaseTest, LargeIndex) {
  auto expr = col_idx(999999);
  EXPECT_EQ(expr->ToString(), "#999999");
}
