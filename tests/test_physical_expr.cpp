// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_physical_expr.cpp
/// @brief Unit tests for physical expressions and CreatePhysicalExpr.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/execution/kernels/batch_kernels.hpp"
#include "quest/execution/lowering/plan_to_graph.hpp"
#include "quest/plan/logical/logical_expr.hpp"
#include "quest/plan/logical/logical_plan.hpp"
#include "quest/plan/physical/expressions/physical_expr.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::columnar;
using namespace quest::plan::physical;
using namespace quest::plan::logical;
using namespace quest::execution;
using quest::testing::MakeColumn;

// =============================================================================
// Test Helpers
// =============================================================================

namespace {

RecordBatch MakeTestBatch() {
  auto col_a = MakeColumn<int32_t>(DataType::kInt32, {10, 20, 30, 40, 50});

  auto col_b = MakeColumn<double>(DataType::kDouble, {1.0, 2.0, 3.0, 4.0, 5.0});

  auto col_c = MakeColumn<bool>(DataType::kBool, {true, false, true, false, true});

  std::vector<Field> fields = {
      Field("a", DataType::kInt32),
      Field("b", DataType::kDouble),
      Field("c", DataType::kBool),
  };
  Schema schema(fields);

  std::vector<ColumnVectorPtr> columns;
  columns.push_back(std::move(col_a));
  columns.push_back(std::move(col_b));
  columns.push_back(std::move(col_c));

  return RecordBatch(schema, std::move(columns));
}

RecordBatch MakeNullableBatch() {
  auto col = MakeColumn<int32_t>(DataType::kInt32, {10, std::nullopt, 30, std::nullopt, 50});

  std::vector<Field> fields = {Field("a", DataType::kInt32, Repetition::kOptional)};
  Schema schema(fields);

  std::vector<ColumnVectorPtr> columns;
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
  BinaryExpression expr(BinaryOp::kEq, std::move(left), std::move(right));

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
  BinaryExpression expr(BinaryOp::kNeq, std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));   // 10 != 30
  EXPECT_FALSE(std::get<bool>(result->GetValue(2)));  // 30 != 30
}

TEST(LtExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{30});
  BinaryExpression expr(BinaryOp::kLt, std::move(left), std::move(right));

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
  BinaryExpression expr(BinaryOp::kGt, std::move(left), std::move(right));

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
  BinaryExpression expr(BinaryOp::kAdd, std::move(left), std::move(right));

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
  BinaryExpression expr(BinaryOp::kSubtract, std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a - 5: [5, 15, 25, 35, 45]; int32 - int32 stays int32
  EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 5);
  EXPECT_EQ(std::get<int32_t>(result->GetValue(4)), 45);
}

TEST(MultiplyExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{2});
  BinaryExpression expr(BinaryOp::kMultiply, std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a * 2: [20, 40, 60, 80, 100]
  EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 20);
  EXPECT_EQ(std::get<int32_t>(result->GetValue(4)), 100);
}

TEST(DivideExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{10});
  BinaryExpression expr(BinaryOp::kDivide, std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a / 10: [1, 2, 3, 4, 5]
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 1.0, 0.001);
  EXPECT_NEAR(std::get<double>(result->GetValue(4)), 5.0, 0.001);
}

TEST(DivideExpressionTest, DivisionByZero) {
  auto left = std::make_unique<LiteralExpression>(Value{10});
  auto right = std::make_unique<LiteralExpression>(Value{0});
  BinaryExpression expr(BinaryOp::kDivide, std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // Division by zero is null, as in DuckDB.
  EXPECT_TRUE(result->IsNull(0));
}

TEST(ModuloExpressionTest, Basic) {
  auto left = std::make_unique<ColumnExpression>(0);
  auto right = std::make_unique<LiteralExpression>(Value{15});
  BinaryExpression expr(BinaryOp::kModulo, std::move(left), std::move(right));

  auto batch = MakeTestBatch();
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // a % 15: [10, 5, 0, 10, 5]
  EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 10);
  EXPECT_EQ(std::get<int32_t>(result->GetValue(2)), 0);
}

// =============================================================================
// Logical Expression Tests
// =============================================================================

TEST(AndExpressionTest, Basic) {
  // c AND c (both true where c is true)
  auto left = std::make_unique<ColumnExpression>(2);
  auto right = std::make_unique<ColumnExpression>(2);
  BinaryExpression expr(BinaryOp::kAnd, std::move(left), std::move(right));

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
  BinaryExpression expr(BinaryOp::kOr, std::move(left), std::move(right));

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
  std::vector<Field> fields = {Field("a", DataType::kInt32)};
  Schema schema(fields);

  auto col = MakeColumn<int32_t>(DataType::kInt32, {});
  std::vector<ColumnVectorPtr> columns;
  columns.push_back(std::move(col));

  RecordBatch empty_batch(schema, std::move(columns));

  // Test SUM on empty
  auto input = std::make_unique<ColumnExpression>(0);
  SumExpression sum_expr(std::move(input));

  ASSERT_OK_AND_ASSIGN(auto result, sum_expr.Evaluate(empty_batch));
  EXPECT_EQ(std::get<double>(result->GetValue(0)), 0.0);  // SUM of nothing = 0
}

TEST(PhysicalExprEdgeCaseTest, NegativeNumbers) {
  auto col = MakeColumn<int32_t>(DataType::kInt32, {-10, 0, 10});

  std::vector<Field> fields = {Field("a", DataType::kInt32)};
  Schema schema(fields);

  std::vector<ColumnVectorPtr> columns;
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
  SchemaOnlyPlan input(Schema({Field("a", DataType::kInt32), Field("b", DataType::kDouble)}));
  auto batch = MakeTestBatch();

  {
    BinaryExpr add_expr(BinaryOp::kAdd, col("a"), lit(5));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(add_expr, input));
    const auto* binary = dynamic_cast<BinaryExpression*>(phys.get());
    ASSERT_NE(binary, nullptr);
    EXPECT_EQ(binary->op(), BinaryOp::kAdd);
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 15);
  }
  {
    BinaryExpr sub_expr(BinaryOp::kSubtract, col("a"), lit(5));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(sub_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 5);
  }
  {
    BinaryExpr mul_expr(BinaryOp::kMultiply, col("a"), lit(2));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(mul_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 20);
  }
  {
    BinaryExpr div_expr(BinaryOp::kDivide, col("a"), lit(10));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(div_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_NEAR(std::get<double>(result->GetValue(0)), 1.0, 0.001);
  }
  {
    BinaryExpr mod_expr(BinaryOp::kModulo, col("a"), lit(15));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(mod_expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_EQ(std::get<int32_t>(result->GetValue(0)), 10);
  }
}

// =============================================================================
// SQL null semantics
// =============================================================================

namespace {

/// Two nullable bool columns "l", "r" covering all nine {true, false, null} pairs.
RecordBatch MakeTruthTableBatch() {
  const std::optional<bool> kVals[] = {true, false, std::nullopt};
  ColumnBuilder<bool> l(DataType::kBool, 9);
  ColumnBuilder<bool> r(DataType::kBool, 9);
  for (auto lv : kVals) {
    for (auto rv : kVals) {
      lv ? l.Append(*lv) : l.AppendNull();
      rv ? r.Append(*rv) : r.AppendNull();
    }
  }
  Schema schema({Field("l", DataType::kBool, Repetition::kOptional),
                 Field("r", DataType::kBool, Repetition::kOptional)});
  return RecordBatch(schema, {std::move(l).Finish(), std::move(r).Finish()});
}

/// Expects @p col to hold exactly @p expected, with std::nullopt meaning SQL null.
template <typename T>
void ExpectColumn(const ColumnVector& col, const std::vector<std::optional<T>>& expected) {
  ASSERT_EQ(col.size(), expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    Value v = col.GetValue(i);
    if (!expected[i]) {
      EXPECT_TRUE(col.IsNull(i)) << "row " << i;
      EXPECT_TRUE(std::holds_alternative<std::monostate>(v)) << "row " << i;
      continue;
    }
    EXPECT_FALSE(col.IsNull(i)) << "row " << i;
    ASSERT_TRUE(std::holds_alternative<T>(v)) << "row " << i << " holds index " << v.index();
    EXPECT_EQ(std::get<T>(v), *expected[i]) << "row " << i;
  }
}

ExpressionPtr Col(size_t i) { return std::make_unique<ColumnExpression>(i); }
ExpressionPtr Lit(Value v) { return std::make_unique<LiteralExpression>(std::move(v)); }

}  // namespace

TEST(SqlNullTest, AndTruthTable) {
  auto batch = MakeTruthTableBatch();
  BinaryExpression expr(BinaryOp::kAnd, Col(0), Col(1));
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
  // l x r over {true, false, null}: false dominates, otherwise null is contagious.
  ExpectColumn<bool>(*result, {true, false, std::nullopt,
                               false, false, false,
                               std::nullopt, false, std::nullopt});
}

TEST(SqlNullTest, OrTruthTable) {
  auto batch = MakeTruthTableBatch();
  BinaryExpression expr(BinaryOp::kOr, Col(0), Col(1));
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
  // true dominates, otherwise null is contagious.
  ExpectColumn<bool>(*result, {true, true, true,
                               true, false, std::nullopt,
                               true, std::nullopt, std::nullopt});
}

TEST(SqlNullTest, AndOrWithNullLiteral) {
  auto batch = MakeTestBatch();  // c: [true, false, true, false, true]
  BinaryExpression and_expr(BinaryOp::kAnd, Col(2), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto and_result, and_expr.Evaluate(batch));
  ExpectColumn<bool>(*and_result, {std::nullopt, false, std::nullopt, false, std::nullopt});

  BinaryExpression or_expr(BinaryOp::kOr, Col(2), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto or_result, or_expr.Evaluate(batch));
  ExpectColumn<bool>(*or_result, {true, std::nullopt, true, std::nullopt, true});
}

TEST(SqlNullTest, AndOrRejectNonBoolOperand) {
  auto batch = MakeTestBatch();
  BinaryExpression expr(BinaryOp::kAnd, Col(2), Col(0));  // c AND a (int32)
  EXPECT_FALSE(expr.Evaluate(batch).has_value());
}

TEST(SqlNullTest, ComparisonWithNullOperandIsNull) {
  auto batch = MakeNullableBatch();  // a: [10, null, 30, null, 50]
  const BinaryOp kOps[] = {BinaryOp::kEq, BinaryOp::kNeq, BinaryOp::kLt,
                           BinaryOp::kLtEq, BinaryOp::kGt, BinaryOp::kGtEq};
  for (auto op : kOps) {
    SCOPED_TRACE(std::string(plan::ToString(op)));
    BinaryExpression expr(op, Col(0), Lit(Value{20}));
    ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
    EXPECT_TRUE(result->IsNull(1));
    EXPECT_TRUE(result->IsNull(3));
    EXPECT_FALSE(result->IsNull(0));
  }

  BinaryExpression gt(BinaryOp::kGt, Col(0), Lit(Value{20}));
  ASSERT_OK_AND_ASSIGN(auto result, gt.Evaluate(batch));
  ExpectColumn<bool>(*result, {false, std::nullopt, true, std::nullopt, true});
}

TEST(SqlNullTest, ComparisonWithNullLiteralIsNull) {
  auto batch = MakeTestBatch();
  // NULL = NULL is null too, not true.
  BinaryExpression null_eq_null(BinaryOp::kEq, Lit(std::monostate{}), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto r1, null_eq_null.Evaluate(batch));
  ExpectColumn<bool>(*r1, std::vector<std::optional<bool>>(5, std::nullopt));

  BinaryExpression a_neq_null(BinaryOp::kNeq, Col(0), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto r2, a_neq_null.Evaluate(batch));
  ExpectColumn<bool>(*r2, std::vector<std::optional<bool>>(5, std::nullopt));
}

TEST(SqlNullTest, ArithmeticWithNullIsNull) {
  auto batch = MakeNullableBatch();  // a: [10, null, 30, null, 50]
  BinaryExpression add(BinaryOp::kAdd, Col(0), Lit(Value{1}));
  ASSERT_OK_AND_ASSIGN(auto result, add.Evaluate(batch));
  ExpectColumn<int32_t>(*result, {11, std::nullopt, 31, std::nullopt, 51});

  BinaryExpression add_null(BinaryOp::kAdd, Col(0), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto all_null, add_null.Evaluate(batch));
  EXPECT_EQ(all_null->GetType(), DataType::kInt32);
  ExpectColumn<int32_t>(*all_null, std::vector<std::optional<int32_t>>(5, std::nullopt));
}

TEST(SqlNullTest, IntArithmeticStaysIntegral) {
  auto batch = MakeTestBatch();  // a: int32 [10..50], b: double [1..5]
  struct Case {
    BinaryOp op;
    ExpressionPtr left;
    ExpressionPtr right;
    DataType type;
    Value first;
  };
  Case cases[] = {
      {BinaryOp::kAdd, Col(0), Col(0), DataType::kInt32, Value{int32_t{20}}},
      {BinaryOp::kSubtract, Col(0), Lit(Value{5}), DataType::kInt32, Value{int32_t{5}}},
      {BinaryOp::kMultiply, Col(0), Lit(Value{int64_t{3}}), DataType::kInt64, Value{int64_t{30}}},
      {BinaryOp::kModulo, Col(0), Lit(Value{3}), DataType::kInt32, Value{int32_t{1}}},
      // int / int is real division, as in DuckDB.
      {BinaryOp::kDivide, Col(0), Lit(Value{4}), DataType::kDouble, Value{2.5}},
      {BinaryOp::kAdd, Col(0), Col(1), DataType::kDouble, Value{11.0}},
  };
  for (auto& c : cases) {
    BinaryExpression expr(c.op, std::move(c.left), std::move(c.right));
    SCOPED_TRACE(expr.ToString());
    ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
    EXPECT_EQ(result->GetType(), c.type);
    EXPECT_EQ(result->GetValue(0), c.first);
  }
}

TEST(SqlNullTest, SmallIntLiteralsAreNumeric) {
  SchemaOnlyPlan input(Schema({Field("a", DataType::kInt32)}));
  auto batch = MakeTestBatch();
  BinaryExpr expr(BinaryOp::kAdd, lit(int8_t{1}), lit(int16_t{2}));
  ASSERT_OK_AND_ASSIGN(auto field, expr.ToField(input));
  EXPECT_TRUE(field.data_type == DataType::kInt16);
  ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(expr, input));
  ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
  EXPECT_EQ(result->GetValue(0), Value{int16_t{3}});
}

TEST(SqlNullTest, DivisionAndModuloByZeroAreNull) {
  auto batch = MakeTestBatch();
  const BinaryOp kOps[] = {BinaryOp::kDivide, BinaryOp::kModulo};
  for (auto op : kOps) {
    SCOPED_TRACE(std::string(plan::ToString(op)));
    BinaryExpression ints(op, Col(0), Lit(Value{0}));
    ASSERT_OK_AND_ASSIGN(auto r1, ints.Evaluate(batch));
    EXPECT_TRUE(r1->IsNull(0));
    BinaryExpression doubles(op, Col(1), Lit(Value{0.0}));
    ASSERT_OK_AND_ASSIGN(auto r2, doubles.Evaluate(batch));
    EXPECT_TRUE(r2->IsNull(0));
  }
}

TEST(SqlNullTest, IntegerOverflowIsAnError) {
  auto batch = MakeTestBatch();
  BinaryExpression expr(BinaryOp::kAdd, Lit(Value{std::numeric_limits<int32_t>::max()}),
                        Lit(Value{1}));
  EXPECT_FALSE(expr.Evaluate(batch).has_value());
}

TEST(SqlNullTest, ArithmeticRejectsNonNumericOperand) {
  auto batch = MakeTestBatch();
  BinaryExpression on_string(BinaryOp::kAdd, Col(0), Lit(Value{std::string("x")}));
  EXPECT_FALSE(on_string.Evaluate(batch).has_value());
  BinaryExpression on_bool(BinaryOp::kAdd, Col(0), Col(2));
  EXPECT_FALSE(on_bool.Evaluate(batch).has_value());
}

TEST(SqlNullTest, FilterDropsNullPredicateRows) {
  auto batch = MakeNullableBatch();  // a: [10, null, 30, null, 50]
  BinaryExpression pred(BinaryOp::kGt, Col(0), Lit(Value{20}));
  ASSERT_OK_AND_ASSIGN(auto out, FilterRecordBatch(batch, pred));
  ExpectColumn<int32_t>(out.column(0), {30, 50});

  // NOT selected is not the same as false: (a > 20) OR NULL keeps only the true rows.
  BinaryExpression or_null(BinaryOp::kOr, std::make_unique<BinaryExpression>(
                                              BinaryOp::kGt, Col(0), Lit(Value{20})),
                           Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto out2, FilterRecordBatch(batch, or_null));
  ASSERT_EQ(out2.num_rows(), 2U);
  ExpectColumn<int32_t>(out2.column(0), {30, 50});
}

TEST(SqlNullTest, FilterPreservesNullsInSelectedRows) {
  auto batch = MakeNullableBatch();  // a: [10, null, 30, null, 50]
  LiteralExpression keep_all(Value{true});
  ASSERT_OK_AND_ASSIGN(auto out, FilterRecordBatch(batch, keep_all));
  ExpectColumn<int32_t>(out.column(0), {10, std::nullopt, 30, std::nullopt, 50});
}

TEST(SqlNullTest, LogicalResultTypeMatchesPhysical) {
  SchemaOnlyPlan input(Schema({Field("a", DataType::kInt32), Field("b", DataType::kDouble),
                               Field("c", DataType::kBool)}));
  auto batch = MakeTestBatch();
  const BinaryOp kOps[] = {BinaryOp::kEq,  BinaryOp::kNeq,      BinaryOp::kLt,
                           BinaryOp::kLtEq, BinaryOp::kGt,      BinaryOp::kGtEq,
                           BinaryOp::kAdd, BinaryOp::kSubtract, BinaryOp::kMultiply,
                           BinaryOp::kDivide, BinaryOp::kModulo};
  auto operands = [] {
    std::vector<std::pair<LogicalExprPtr, LogicalExprPtr>> v;
    v.emplace_back(col("a"), col("a"));
    v.emplace_back(col("a"), col("b"));
    v.emplace_back(col("b"), col("a"));
    v.emplace_back(col("a"), lit(int64_t{7}));
    v.emplace_back(col("a"), lit(Value{std::monostate{}}));
    return v;
  };
  for (auto op : kOps) {
    for (auto& [l, r] : operands()) {
      BinaryExpr expr(op, std::move(l), std::move(r));
      SCOPED_TRACE(expr.ToString());
      ASSERT_OK_AND_ASSIGN(auto field, expr.ToField(input));
      ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(expr, input));
      ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
      EXPECT_EQ(field.data_type, result->GetType());
    }
  }

  BinaryExpr and_expr(BinaryOp::kAnd, col("c"), col("c"));
  ASSERT_OK_AND_ASSIGN(auto and_field, and_expr.ToField(input));
  EXPECT_TRUE(and_field.data_type == DataType::kBool);

  BinaryExpr int_div(BinaryOp::kDivide, col("a"), col("a"));
  ASSERT_OK_AND_ASSIGN(auto div_field, int_div.ToField(input));
  EXPECT_TRUE(div_field.data_type == DataType::kDouble);

  BinaryExpr bad(BinaryOp::kAdd, col("a"), col("c"));
  EXPECT_FALSE(bad.ToField(input).has_value());
}

// =============================================================================
// CASE expression
// =============================================================================

namespace {

ExpressionPtr Case(ExpressionPtr when, ExpressionPtr then, ExpressionPtr otherwise) {
  std::vector<std::pair<ExpressionPtr, ExpressionPtr>> branches;
  branches.emplace_back(std::move(when), std::move(then));
  return std::make_unique<CaseExpression>(std::move(branches), std::move(otherwise));
}

ExpressionPtr Gt(ExpressionPtr l, ExpressionPtr r) {
  return std::make_unique<BinaryExpression>(BinaryOp::kGt, std::move(l), std::move(r));
}

/// o_orderpriority: ["1-URGENT", "2-HIGH", "3-MEDIUM", null, "5-LOW", "2-HIGH"].
RecordBatch MakePriorityBatch() {
  auto p = MakeColumn<std::string_view>(DataType::kString, {"1-URGENT", "2-HIGH", "3-MEDIUM", std::nullopt, "5-LOW", "2-HIGH"});
  return RecordBatch(Schema({Field("o_orderpriority", DataType::kString, Repetition::kOptional)}), {p});
}

}  // namespace

TEST(CaseExpressionTest, NullWhenTakesElse) {
  auto batch = MakeTruthTableBatch();  // l: [t, t, t, f, f, f, null, null, null]
  auto expr = Case(Col(0), Lit(Value{1}), Lit(Value{0}));
  ASSERT_OK_AND_ASSIGN(auto result, expr->Evaluate(batch));
  EXPECT_EQ(result->GetType(), DataType::kInt32);
  ExpectColumn<int32_t>(*result, {1, 1, 1, 0, 0, 0, 0, 0, 0});
}

TEST(CaseExpressionTest, NullLiteralWhenTakesElse) {
  auto batch = MakeTestBatch();
  auto expr = Case(Lit(std::monostate{}), Lit(Value{1}), Lit(Value{0}));
  ASSERT_OK_AND_ASSIGN(auto result, expr->Evaluate(batch));
  ExpectColumn<int32_t>(*result, {0, 0, 0, 0, 0});
}

TEST(CaseExpressionTest, NullsInChosenBranchArePreserved) {
  auto batch = MakeNullableBatch();  // a: [10, null, 30, null, 50]
  auto then_a = Case(Gt(Col(0), Lit(Value{20})), Col(0), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto r1, then_a->Evaluate(batch));
  EXPECT_EQ(r1->GetType(), DataType::kInt32);
  ExpectColumn<int32_t>(*r1, {std::nullopt, std::nullopt, 30, std::nullopt, 50});

  auto else_a = Case(Gt(Col(0), Lit(Value{20})), Lit(std::monostate{}), Col(0));
  ASSERT_OK_AND_ASSIGN(auto r2, else_a->Evaluate(batch));
  EXPECT_EQ(r2->GetType(), DataType::kInt32);
  ExpectColumn<int32_t>(*r2, {10, std::nullopt, std::nullopt, std::nullopt, std::nullopt});

  // A null in the branch that is not chosen does not leak into the result.
  auto expr = Case(Gt(Col(0), Lit(Value{20})), Lit(Value{1}), Col(0));
  ASSERT_OK_AND_ASSIGN(auto r3, expr->Evaluate(batch));
  ExpectColumn<int32_t>(*r3, {10, std::nullopt, 1, std::nullopt, 1});
}

TEST(CaseExpressionTest, BranchesWidenToCommonType) {
  auto batch = MakeTestBatch();  // a: int32 [10..50], b: double [1..5], c: [t, f, t, f, t]
  auto same = Case(Col(2), Col(0), Col(0));
  ASSERT_OK_AND_ASSIGN(auto r1, same->Evaluate(batch));
  EXPECT_EQ(r1->GetType(), DataType::kInt32);
  ExpectColumn<int32_t>(*r1, {10, 20, 30, 40, 50});

  auto int64 = Case(Col(2), Col(0), Lit(Value{int64_t{7}}));
  ASSERT_OK_AND_ASSIGN(auto r2, int64->Evaluate(batch));
  EXPECT_EQ(r2->GetType(), DataType::kInt64);
  ExpectColumn<int64_t>(*r2, {10, 7, 30, 7, 50});

  auto dbl = Case(Col(2), Col(0), Col(1));
  ASSERT_OK_AND_ASSIGN(auto r3, dbl->Evaluate(batch));
  EXPECT_EQ(r3->GetType(), DataType::kDouble);
  ExpectColumn<double>(*r3, {10.0, 2.0, 30.0, 4.0, 50.0});

  auto strings = Case(Col(2), Lit(Value{std::string("y")}), Lit(Value{std::string("n")}));
  ASSERT_OK_AND_ASSIGN(auto r4, strings->Evaluate(batch));
  EXPECT_EQ(r4->GetType(), DataType::kString);
  ExpectColumn<std::string>(*r4, {"y", "n", "y", "n", "y"});

  auto bools = Case(Col(2), Lit(Value{false}), Col(2));
  ASSERT_OK_AND_ASSIGN(auto r5, bools->Evaluate(batch));
  EXPECT_EQ(r5->GetType(), DataType::kBool);
  ExpectColumn<bool>(*r5, {false, false, false, false, false});
}

TEST(CaseExpressionTest, TypeErrors) {
  auto batch = MakeTestBatch();
  auto non_bool_when = Case(Col(0), Lit(Value{1}), Lit(Value{0}));
  auto r1 = non_bool_when->Evaluate(batch);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error().code(), ErrorCode::TypeError);

  auto int_vs_string = Case(Col(2), Col(0), Lit(Value{std::string("x")}));
  auto r2 = int_vs_string->Evaluate(batch);
  ASSERT_FALSE(r2.has_value());
  EXPECT_EQ(r2.error().code(), ErrorCode::TypeError);

  auto bool_vs_int = Case(Col(2), Col(2), Col(0));
  auto r3 = bool_vs_int->Evaluate(batch);
  ASSERT_FALSE(r3.has_value());
  EXPECT_EQ(r3.error().code(), ErrorCode::TypeError);
}

TEST(CaseExpressionTest, ToString) {
  auto expr = Case(Col(2), Lit(Value{1}), Lit(Value{0}));
  EXPECT_EQ(expr->ToString(), "CASE WHEN #2 THEN 1 ELSE 0 END");
}

TEST(CaseExpressionTest, EmptyBatch) {
  RecordBatch empty(Schema({Field("c", DataType::kBool)}),
                    {MakeColumn<bool>(DataType::kBool, {})});
  auto expr = Case(Col(0), Lit(Value{1}), Col(0));
  ASSERT_FALSE(expr->Evaluate(empty).has_value());  // int32 vs bool is still a type error
  auto ok = Case(Col(0), Lit(Value{1}), Lit(Value{0}));
  ASSERT_OK_AND_ASSIGN(auto result, ok->Evaluate(empty));
  EXPECT_EQ(result->size(), 0U);
}

TEST(CaseExpressionTest, Q12PriorityFlag) {
  // CASE WHEN o_orderpriority = '1-URGENT' OR o_orderpriority = '2-HIGH' THEN 1 ELSE 0 END
  auto batch = MakePriorityBatch();
  auto is = [](const char* s) {
    return std::make_unique<BinaryExpression>(BinaryOp::kEq, Col(0), Lit(Value{std::string(s)}));
  };
  auto isnt = [](const char* s) {
    return std::make_unique<BinaryExpression>(BinaryOp::kNeq, Col(0), Lit(Value{std::string(s)}));
  };
  auto high = Case(std::make_unique<BinaryExpression>(BinaryOp::kOr, is("1-URGENT"), is("2-HIGH")),
                   Lit(Value{1}), Lit(Value{0}));
  ASSERT_OK_AND_ASSIGN(auto high_result, high->Evaluate(batch));
  EXPECT_EQ(high_result->GetType(), DataType::kInt32);
  ExpectColumn<int32_t>(*high_result, {1, 1, 0, 0, 0, 1});

  auto low =
      Case(std::make_unique<BinaryExpression>(BinaryOp::kAnd, isnt("1-URGENT"), isnt("2-HIGH")),
           Lit(Value{1}), Lit(Value{0}));
  ASSERT_OK_AND_ASSIGN(auto low_result, low->Evaluate(batch));
  ExpectColumn<int32_t>(*low_result, {0, 0, 1, 0, 1, 0});
}

TEST(CaseExprTest, ToString) {
  auto expr = case_when(eq(col("p"), lit(std::string("1-URGENT"))), lit(1), lit(0));
  EXPECT_EQ(expr->ToString(), "CASE WHEN (#p == \"1-URGENT\") THEN 1 ELSE 0 END");
  EXPECT_EQ(expr->Clone()->ToString(), expr->ToString());
}

TEST(CaseExprTest, ResultTypeIsCommonTypeOfBranches) {
  SchemaOnlyPlan input(Schema({Field("a", DataType::kInt32), Field("b", DataType::kDouble),
                               Field("c", DataType::kBool), Field("s", DataType::kString)}));
  struct Want {
    LogicalExprPtr then;
    LogicalExprPtr otherwise;
    DataType type;
  };
  Want cases[] = {
      {col("a"), col("a"), DataType::kInt32},
      {col("a"), lit(int64_t{7}), DataType::kInt64},
      {lit(int8_t{1}), lit(int16_t{2}), DataType::kInt16},
      {col("a"), col("b"), DataType::kDouble},
      {lit(1.5F), col("a"), DataType::kDouble},
      {lit(1.5F), lit(2.5F), DataType::kFloat},
      {col("a"), lit(Value{std::monostate{}}), DataType::kInt32},
      {lit(Value{std::monostate{}}), col("b"), DataType::kDouble},
      {col("s"), lit(std::string("x")), DataType::kString},
      {col("c"), lit(false), DataType::kBool},
  };
  for (auto& c : cases) {
    std::vector<std::pair<LogicalExprPtr, LogicalExprPtr>> branches;
    branches.emplace_back(col("c"), std::move(c.then));
    CaseExpr expr(std::move(branches), std::move(c.otherwise));
    SCOPED_TRACE(expr.ToString());
    ASSERT_OK_AND_ASSIGN(auto field, expr.ToField(input));
    EXPECT_EQ(field.data_type, c.type);
    EXPECT_EQ(field.name, expr.ToString());
  }
}

TEST(CaseExprTest, TypeErrors) {
  SchemaOnlyPlan input(
      Schema({Field("a", DataType::kInt32), Field("c", DataType::kBool), Field("s", DataType::kString)}));
  auto expect_type_error = [&](const LogicalExprPtr& expr) {
    SCOPED_TRACE(expr->ToString());
    auto field = expr->ToField(input);
    ASSERT_FALSE(field.has_value());
    EXPECT_EQ(field.error().code(), ErrorCode::TypeError);
  };
  expect_type_error(case_when(col("a"), lit(1), lit(0)));
  expect_type_error(case_when(col("s"), lit(1), lit(0)));
  expect_type_error(case_when(col("c"), col("a"), col("s")));
  expect_type_error(case_when(col("c"), col("c"), col("a")));

  auto missing = case_when(col("c"), col("nope"), lit(0))->ToField(input);
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::SchemaError);
}

TEST(CaseExprTest, LogicalTypeMatchesPhysical) {
  SchemaOnlyPlan input(
      Schema({Field("a", DataType::kInt32), Field("b", DataType::kDouble), Field("c", DataType::kBool)}));
  auto batch = MakeTestBatch();
  auto branches = [] {
    std::vector<std::pair<LogicalExprPtr, LogicalExprPtr>> v;
    v.emplace_back(col("a"), col("a"));
    v.emplace_back(col("a"), lit(int64_t{7}));
    v.emplace_back(col("b"), col("a"));
    v.emplace_back(lit(Value{std::monostate{}}), col("a"));
    v.emplace_back(col("c"), lit(true));
    return v;
  };
  for (auto& [then, otherwise] : branches()) {
    auto expr = case_when(gt(col("a"), lit(20)), std::move(then), std::move(otherwise));
    SCOPED_TRACE(expr->ToString());
    ASSERT_OK_AND_ASSIGN(auto field, expr->ToField(input));
    ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(*expr, input));
    ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(batch));
    EXPECT_EQ(field.data_type, result->GetType());
  }
}

TEST(CaseExprTest, LoweredQ12PriorityFlag) {
  SchemaOnlyPlan input(Schema({Field("o_orderpriority", DataType::kString, Repetition::kOptional)}));
  auto expr = case_when(or_expr(eq(col("o_orderpriority"), lit(std::string("1-URGENT"))),
                                eq(col("o_orderpriority"), lit(std::string("2-HIGH")))),
                        lit(1), lit(0));
  ASSERT_OK_AND_ASSIGN(auto field, expr->ToField(input));
  EXPECT_TRUE(field.data_type == DataType::kInt32);
  ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(*expr, input));
  ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(MakePriorityBatch()));
  ExpectColumn<int32_t>(*result, {1, 1, 0, 0, 0, 1});
}

// =============================================================================
// Date32
// =============================================================================

namespace {

constexpr Date32 k19940101{8766};
constexpr Date32 k19941231{9130};
constexpr Date32 k19950615{9296};

/// a: [1994-01-01, null, 1995-06-15], b: [1994-01-01, 1994-01-01, 1994-12-31], i: int32
RecordBatch MakeDateBatch() {
  auto a = MakeColumn<Date32>(DataType::kDate32, {k19940101, std::nullopt, k19950615});
  auto b = MakeColumn<Date32>(DataType::kDate32, {k19940101, k19940101, k19941231});
  auto i = MakeColumn<int32_t>(DataType::kInt32, {8766, 8766, 9130});
  return RecordBatch(Schema({Field("a", DataType::kDate32, Repetition::kOptional),
                             Field("b", DataType::kDate32), Field("i", DataType::kInt32)}),
                     {a, b, i});
}

}  // namespace

TEST(Date32ExprTest, ComparisonsFollowCalendarOrderAndNullSemantics) {
  auto batch = MakeDateBatch();
  struct Case {
    BinaryOp op;
    std::vector<std::optional<bool>> expected;
  };
  const Case kCases[] = {
      {BinaryOp::kEq, {true, std::nullopt, false}},
      {BinaryOp::kNeq, {false, std::nullopt, true}},
      {BinaryOp::kLt, {false, std::nullopt, false}},
      {BinaryOp::kLtEq, {true, std::nullopt, false}},
      {BinaryOp::kGt, {false, std::nullopt, true}},
      {BinaryOp::kGtEq, {true, std::nullopt, true}},
  };
  for (const auto& c : kCases) {
    BinaryExpression expr(c.op, Col(0), Col(1));
    SCOPED_TRACE(expr.ToString());
    ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
    ExpectColumn<bool>(*result, c.expected);
  }
}

TEST(Date32ExprTest, ComparisonWithNullLiteralIsNull) {
  auto batch = MakeDateBatch();
  BinaryExpression expr(BinaryOp::kLt, Col(1), Lit(std::monostate{}));
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));
  ExpectColumn<bool>(*result, std::vector<std::optional<bool>>(3, std::nullopt));
}

TEST(Date32ExprTest, ComparisonWithNonDateIsTypeError) {
  auto batch = MakeDateBatch();
  BinaryExpression date_int(BinaryOp::kEq, Col(1), Col(2));
  auto r1 = date_int.Evaluate(batch);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error().code(), ErrorCode::TypeError);

  BinaryExpression int_date(BinaryOp::kLt, Lit(Value{8766}), Col(0));
  EXPECT_FALSE(int_date.Evaluate(batch).has_value());

  BinaryExpression date_string(BinaryOp::kEq, Col(0), Lit(Value{std::string("1994-01-01")}));
  EXPECT_FALSE(date_string.Evaluate(batch).has_value());
}

TEST(Date32ExprTest, ArithmeticOnDatesIsTypeError) {
  auto batch = MakeDateBatch();
  BinaryExpression date_date(BinaryOp::kSubtract, Col(0), Col(1));
  auto r1 = date_date.Evaluate(batch);
  ASSERT_FALSE(r1.has_value());
  EXPECT_EQ(r1.error().code(), ErrorCode::TypeError);

  BinaryExpression date_int(BinaryOp::kAdd, Col(1), Lit(Value{1}));
  EXPECT_FALSE(date_int.Evaluate(batch).has_value());
}

TEST(TimestampExprTest, ComparesOnlyWithTimestamps) {
  auto ts = MakeColumn<Timestamp>(DataType::kTimestamp, {Timestamp{-1}, Timestamp{1'704'164'645'500'000}});
  auto date = MakeColumn<Date32>(DataType::kDate32, {Date32{0}, Date32{19724}});
  RecordBatch batch(Schema({Field("ts", DataType::kTimestamp), Field("d", DataType::kDate32)}),
                    {ts, date});

  BinaryExpression later(BinaryOp::kLt, Lit(Value{Timestamp{0}}), Col(0));
  ASSERT_OK_AND_ASSIGN(auto result, later.Evaluate(batch));
  ExpectColumn<bool>(*result, {false, true});
  EXPECT_EQ(later.ToString(), "TIMESTAMP '1970-01-01 00:00:00' < #0");

  BinaryExpression ts_date(BinaryOp::kEq, Col(0), Col(1));
  auto mixed = ts_date.Evaluate(batch);
  ASSERT_FALSE(mixed.has_value());
  EXPECT_EQ(mixed.error().code(), ErrorCode::TypeError);

  BinaryExpression ts_plus(BinaryOp::kAdd, Col(0), Lit(Value{1}));
  EXPECT_FALSE(ts_plus.Evaluate(batch).has_value());
}

TEST(Date32ExprTest, LogicalTypesMatchPhysicalRules) {
  SchemaOnlyPlan input(Schema({Field("a", DataType::kDate32, Repetition::kOptional),
                               Field("b", DataType::kDate32), Field("i", DataType::kInt32)}));
  BinaryExpr cmp(BinaryOp::kLtEq, col("a"), col("b"));
  ASSERT_OK_AND_ASSIGN(auto cmp_field, cmp.ToField(input));
  EXPECT_TRUE(cmp_field.data_type == DataType::kBool);

  BinaryExpr cmp_null(BinaryOp::kEq, col("a"), lit(Value{std::monostate{}}));
  EXPECT_TRUE(cmp_null.ToField(input).has_value());

  BinaryExpr mixed(BinaryOp::kEq, col("a"), col("i"));
  EXPECT_FALSE(mixed.ToField(input).has_value());

  BinaryExpr arith(BinaryOp::kSubtract, col("a"), col("b"));
  EXPECT_FALSE(arith.ToField(input).has_value());
}

TEST(Date32ExprTest, LitDateIsATypedDate32Literal) {
  auto built = LitDate(std::chrono::year{1994} / 1 / 1).Build();
  const auto* literal = dynamic_cast<const Literal*>(built.get());
  ASSERT_NE(literal, nullptr);
  EXPECT_EQ(literal->value(), Value{k19940101});
  EXPECT_EQ(literal->ToString(), "DATE '1994-01-01'");

  SchemaOnlyPlan input(Schema({Field("b", DataType::kDate32)}));
  ASSERT_OK_AND_ASSIGN(auto field, literal->ToField(input));
  EXPECT_TRUE(field.data_type == DataType::kDate32);

  ASSERT_OK_AND_ASSIGN(auto phys, CreatePhysicalExpr(*literal, input));
  EXPECT_EQ(phys->ToString(), "DATE '1994-01-01'");
  ASSERT_OK_AND_ASSIGN(auto result, phys->Evaluate(MakeDateBatch()));
  EXPECT_EQ(result->GetType(), DataType::kDate32);
  EXPECT_EQ(result->GetValue(2), Value{k19940101});

  EXPECT_EQ(LitDate(std::chrono::year{2000} / 2 / 29).GetExpr().ToString(), "DATE '2000-02-29'");
}

TEST(Date32ExprTest, FilterKeepsDate32Columns) {
  auto batch = MakeDateBatch();
  BinaryExpression pred(BinaryOp::kGtEq, Col(1), Lit(Value{k19941231}));
  ASSERT_OK_AND_ASSIGN(auto out, FilterRecordBatch(batch, pred));
  ASSERT_EQ(out.num_rows(), 1U);
  EXPECT_EQ(out.column(0).GetType(), DataType::kDate32);
  ExpectColumn<Date32>(out.column(0), {k19950615});
  ExpectColumn<Date32>(out.column(1), {k19941231});
}
