// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_physical_expr.cpp
/// @brief Unit tests for physical expressions and CreatePhysicalExpr.

#include <gtest/gtest.h>

#include <cmath>
#include <memory>
#include <vector>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/execution/graph_builder.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/expressions/physical_expr.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::datasource;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

RecordBatch MakeTestBatch() {
  auto col_a = std::make_unique<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  col_a->Append(10);
  col_a->Append(20);
  col_a->Append(30);
  col_a->Append(40);
  col_a->Append(50);

  auto col_b = std::make_unique<TypedColumnVector<double>>(DoubleType{}, 5);
  col_b->Append(1.0);
  col_b->Append(2.0);
  col_b->Append(3.0);
  col_b->Append(4.0);
  col_b->Append(5.0);

  auto col_c = std::make_unique<TypedColumnVector<bool>>(BoolType{}, 5);
  col_c->Append(true);
  col_c->Append(false);
  col_c->Append(true);
  col_c->Append(false);
  col_c->Append(true);

  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", DoubleType{}),
      Field("c", BoolType{}),
  };
  Schema schema(fields);

  std::vector<std::shared_ptr<ColumnVector>> columns;
  columns.push_back(std::move(col_a));
  columns.push_back(std::move(col_b));
  columns.push_back(std::move(col_c));

  return RecordBatch(schema, std::move(columns));
}

RecordBatch MakeNullableBatch() {
  auto col = std::make_unique<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  col->Append(10);
  col->AppendNull();
  col->Append(30);
  col->AppendNull();
  col->Append(50);

  std::vector<Field> fields = {Field("a", Int32Type{}, Repetition::kOptional)};
  Schema schema(fields);

  std::vector<std::shared_ptr<ColumnVector>> columns;
  columns.push_back(std::move(col));

  return RecordBatch(schema, std::move(columns));
}

}  // namespace

// =============================================================================
// LiteralExpression Tests
// =============================================================================

TEST(LiteralExpressionTest, IntLiteral) {
  LiteralExpression expr(Value{42});
  auto batch = MakeTestBatch();

  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_EQ(result->size(), batch.num_rows());
  EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 42);
  EXPECT_EQ(std::get<int32_t>(result->GetValue(4)), 42);
}

TEST(LiteralExpressionTest, DoubleLiteral) {
  LiteralExpression expr(Value{3.14});
  auto batch = MakeTestBatch();

  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_EQ(result->size(), batch.num_rows());
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 3.14, 0.001);
}

TEST(LiteralExpressionTest, BoolLiteral) {
  LiteralExpression expr(Value{true});
  auto batch = MakeTestBatch();

  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));
}

TEST(LiteralExpressionTest, ToString) {
  LiteralExpression int_expr(Value{42});
  LiteralExpression double_expr(Value{3.14});
  LiteralExpression bool_expr(Value{true});

  EXPECT_EQ(int_expr.ToString(), "42");
  EXPECT_NE(double_expr.ToString().find("3.14"), std::string::npos);
  EXPECT_EQ(bool_expr.ToString(), "true");
}

// =============================================================================
// ColumnExpression Tests
// =============================================================================

TEST(ColumnExpressionTest, GetColumn) {
  ColumnExpression expr(0);  // Column "a"
  auto batch = MakeTestBatch();

  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_EQ(result->size(), 5U);
  EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 10);
  EXPECT_EQ(std::get<int32_t>(result->GetValue(4)), 50);
}

TEST(ColumnExpressionTest, OutOfRange) {
  ColumnExpression expr(99);  // Invalid index
  auto batch = MakeTestBatch();

  auto result = expr.Evaluate(batch);
  EXPECT_FALSE(result.has_value());
}

TEST(ColumnExpressionTest, ToString) {
  ColumnExpression expr(5);
  EXPECT_EQ(expr.ToString(), "#5");
}

// =============================================================================
// Comparison Expression Tests
// =============================================================================

TEST(EqExpressionTest, IntEquality) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{30});
  EqExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a: [10, 20, 30, 40, 50] == 30 -> [false, false, true, false, false]
  EXPECT_FALSE(std::get<bool>(result->GetValue(0)));
  EXPECT_FALSE(std::get<bool>(result->GetValue(1)));
  EXPECT_TRUE(std::get<bool>(result->GetValue(2)));
  EXPECT_FALSE(std::get<bool>(result->GetValue(3)));
  EXPECT_FALSE(std::get<bool>(result->GetValue(4)));
}

TEST(NeqExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{30});
  NeqExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));   // 10 != 30
  EXPECT_FALSE(std::get<bool>(result->GetValue(2)));  // 30 != 30
}

TEST(LtExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{30});
  LtExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a: [10, 20, 30, 40, 50] < 30 -> [true, true, false, false, false]
  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));
  EXPECT_TRUE(std::get<bool>(result->GetValue(1)));
  EXPECT_FALSE(std::get<bool>(result->GetValue(2)));
}

TEST(GtExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{30});
  GtExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a: [10, 20, 30, 40, 50] > 30 -> [false, false, false, true, true]
  EXPECT_FALSE(std::get<bool>(result->GetValue(2)));
  EXPECT_TRUE(std::get<bool>(result->GetValue(3)));
  EXPECT_TRUE(std::get<bool>(result->GetValue(4)));
}

// =============================================================================
// Arithmetic Expression Tests
// =============================================================================

TEST(AddExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);   // a (int32)
  auto right = std::make_unique<ColumnExpression>(1);  // b (double)
  AddExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a + b: [10+1, 20+2, 30+3, 40+4, 50+5] = [11, 22, 33, 44, 55]
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 11.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(2)), 33.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(4)), 55.0, 0.001);
}

TEST(SubtractExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{5});
  SubtractExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a - 5: [5, 15, 25, 35, 45]
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 5.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(4)), 45.0, 0.001);
}

TEST(MultiplyExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{2});
  MultiplyExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a * 2: [20, 40, 60, 80, 100]
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 20.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(4)), 100.0, 0.001);
}

TEST(DivideExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{10});
  DivideExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a / 10: [1, 2, 3, 4, 5]
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 1.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(4)), 5.0, 0.001);
}

TEST(DivideExpressionTest, DivisionByZero) {
  auto left = std::make_unique<LiteralExpression>(Value{10});
  auto right = std::make_unique<LiteralExpression>(Value{0});
  DivideExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // Should return NaN
  EXPECT_TRUE(std::isnan(std::get<double>(result->GetValue(0))));
}

TEST(ModuloExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{15});
  ModuloExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a % 15: [10, 5, 0, 10, 5]
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 10.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(2)), 0.0, 0.001);
}

// =============================================================================
// Logical Expression Tests
// =============================================================================

TEST(AndExpressionTest, Basic) {
  // c AND c (both true where c is true)
  auto left = std::make_unique<ColumnExpression>(2);
  auto right = std::make_unique<ColumnExpression>(2);
  AndExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // c: [true, false, true, false, true]
  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));
  EXPECT_FALSE(std::get<bool>(result->GetValue(1)));
  EXPECT_TRUE(std::get<bool>(result->GetValue(2)));
}

TEST(OrExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(2);
  auto right = std::make_unique<LiteralExpression>(Value{false});
  OrExpression expr(std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // c OR false = c
  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));
  EXPECT_FALSE(std::get<bool>(result->GetValue(1)));
}

// =============================================================================
// Aggregate Expression Tests
// =============================================================================

TEST(SumExpressionTest, Basic) {
  auto input = std::make_unique<ColumnExpression>(0);
  SumExpression expr(std::move(input));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // SUM(a) = 10 + 20 + 30 + 40 + 50 = 150
  EXPECT_EQ(result->size(), 1U);
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 150.0, 0.001);
}

TEST(SumExpressionTest, WithNulls) {
  auto input = std::make_unique<ColumnExpression>(0);
  SumExpression expr(std::move(input));

  auto batch = MakeNullableBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // SUM of [10, null, 30, null, 50] = 90
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 90.0, 0.001);
}

TEST(AvgExpressionTest, Basic) {
  auto input = std::make_unique<ColumnExpression>(0);
  AvgExpression expr(std::move(input));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // AVG(a) = 150 / 5 = 30
  EXPECT_EQ(result->size(), 1U);
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 30.0, 0.001);
}

TEST(AvgExpressionTest, WithNulls) {
  auto input = std::make_unique<ColumnExpression>(0);
  AvgExpression expr(std::move(input));

  auto batch = MakeNullableBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // AVG of [10, null, 30, null, 50] = 90 / 3 = 30
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 30.0, 0.001);
}

TEST(CountExpressionTest, Basic) {
  auto input = std::make_unique<ColumnExpression>(0);
  CountExpression expr(std::move(input));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // COUNT(a) = 5 (all non-null)
  EXPECT_EQ(result->size(), 1U);
  EXPECT_EQ(std::get<int64_t>(result->GetValue(0)), 5);
}

TEST(CountExpressionTest, WithNulls) {
  auto input = std::make_unique<ColumnExpression>(0);
  CountExpression expr(std::move(input));

  auto batch = MakeNullableBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // COUNT of [10, null, 30, null, 50] = 3 (non-null only)
  EXPECT_EQ(std::get<int64_t>(result->GetValue(0)), 3);
}

TEST(CountStarExpressionTest, Basic) {
  CountStarExpression expr;

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // COUNT(*) = 5 (all rows)
  EXPECT_EQ(result->size(), 1U);
  EXPECT_EQ(std::get<int64_t>(result->GetValue(0)), 5);
}

TEST(CountStarExpressionTest, WithNulls) {
  CountStarExpression expr;

  auto batch = MakeNullableBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // COUNT(*) = 5 (includes nulls)
  EXPECT_EQ(std::get<int64_t>(result->GetValue(0)), 5);
}

TEST(MinExpressionTest, Basic) {
  auto input = std::make_unique<ColumnExpression>(0);
  MinExpression expr(std::move(input));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // MIN(a) = 10
  EXPECT_EQ(result->size(), 1U);
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 10.0, 0.001);
}

TEST(MinExpressionTest, WithNulls) {
  auto input = std::make_unique<ColumnExpression>(0);
  MinExpression expr(std::move(input));

  auto batch = MakeNullableBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // MIN of [10, null, 30, null, 50] = 10
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 10.0, 0.001);
}

TEST(MaxExpressionTest, Basic) {
  auto input = std::make_unique<ColumnExpression>(0);
  MaxExpression expr(std::move(input));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // MAX(a) = 50
  EXPECT_EQ(result->size(), 1U);
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 50.0, 0.001);
}

TEST(MaxExpressionTest, WithNulls) {
  auto input = std::make_unique<ColumnExpression>(0);
  MaxExpression expr(std::move(input));

  auto batch = MakeNullableBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // MAX of [10, null, 30, null, 50] = 50
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 50.0, 0.001);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(PhysicalExprEdgeCaseTest, EmptyBatch) {
  std::vector<Field> fields = {Field("a", Int32Type{})};
  Schema schema(fields);

  auto col = std::make_unique<TypedColumnVector<int32_t>>(Int32Type{}, 0);
  std::vector<std::shared_ptr<ColumnVector>> columns;
  columns.push_back(std::move(col));

  RecordBatch empty_batch(schema, std::move(columns));

  // Test SUM on empty
  auto input = std::make_unique<ColumnExpression>(0);
  SumExpression sum_expr(std::move(input));

  ASSERT_OK_AND_ASSIGN(auto result, sum_expr.Evaluate(empty_batch));
  EXPECT_EQ(std::get<double>(result->GetValue(0)), 0.0);  // SUM of nothing = 0
}

TEST(PhysicalExprEdgeCaseTest, NegativeNumbers) {
  auto col = std::make_unique<TypedColumnVector<int32_t>>(Int32Type{}, 3);
  col->Append(-10);
  col->Append(0);
  col->Append(10);

  std::vector<Field> fields = {Field("a", Int32Type{})};
  Schema schema(fields);

  std::vector<std::shared_ptr<ColumnVector>> columns;
  columns.push_back(std::move(col));

  RecordBatch batch(schema, std::move(columns));

  auto input = std::make_unique<ColumnExpression>(0);
  MinExpression expr(std::move(input));

  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), -10.0, 0.001);
}

// =============================================================================
// CreatePhysicalExpr arithmetic wiring (Phase 1)
// =============================================================================

namespace {

class SchemaOnlyPlan : public LogicalPlan {
 public:
  explicit SchemaOnlyPlan(Schema s) : schema_(std::move(s)) {}
  [[nodiscard]] Schema schema() const override { return schema_; }
  [[nodiscard]] std::vector<const LogicalPlan*> Children() const override { return {}; }
  [[nodiscard]] std::string ToString() const override { return "SchemaOnly"; }

 private:
  Schema schema_;
};

}  // namespace

TEST(CreatePhysicalExprTest, ArithmeticOpsRoundTripEvaluate) {
  SchemaOnlyPlan input(Schema({Field("a", Int32Type{}), Field("b", DoubleType{})}));
  auto batch = MakeTestBatch();

  {
    Add add_expr(col("a"), lit(5));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(add_expr, input));
    ASSERT_NE(dynamic_cast<AddExpression*>(phys.get()), nullptr);
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_NEAR(std::get<double>(result->GetValue(0)), 15.0, 0.001);
  }
  {
    Subtract sub_expr(col("a"), lit(5));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(sub_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_NEAR(std::get<double>(result->GetValue(0)), 5.0, 0.001);
  }
  {
    Multiply mul_expr(col("a"), lit(2));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(mul_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_NEAR(std::get<double>(result->GetValue(0)), 20.0, 0.001);
  }
  {
    Divide div_expr(col("a"), lit(10));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(div_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_NEAR(std::get<double>(result->GetValue(0)), 1.0, 0.001);
  }
  {
    Modulo mod_expr(col("a"), lit(15));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(mod_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_NEAR(std::get<double>(result->GetValue(0)), 10.0, 0.001);
  }
}
