// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_expr_builder.cpp
/// @brief Unit tests for ExprBuilder DSL.

#include <gtest/gtest.h>

#include <functional>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/builder/query_plan.hpp"
#include "quest/datasource/record_batch.hpp"

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
// ExprBuilder Rule of Five Tests
// =============================================================================

TEST(ExprBuilderRuleOfFive, CopyConstruction) {
  auto original = Col("id") == Lit(42);
  ExprBuilder copy(original);

  EXPECT_NE(original.Build(), nullptr);
  EXPECT_NE(copy.Build(), nullptr);
  EXPECT_EQ(original.Build()->ToString(), copy.Build()->ToString());
}

TEST(ExprBuilderRuleOfFive, CopyAssignment) {
  auto original = Col("salary") > Lit(50000.0);
  ExprBuilder copy = Col("name");
  copy = original;

  EXPECT_NE(copy.Build(), nullptr);
  EXPECT_EQ(original.Build()->ToString(), copy.Build()->ToString());
}

TEST(ExprBuilderRuleOfFive, MoveConstruction) {
  auto original = Col("id") == Lit(100);
  auto original_str = original.Build()->ToString();

  ExprBuilder moved(std::move(original));

  EXPECT_NE(moved.Build(), nullptr);
  EXPECT_EQ(moved.Build()->ToString(), original_str);
}

TEST(ExprBuilderRuleOfFive, MoveAssignment) {
  auto original = Col("dept") == Lit(std::string("HR"));
  auto original_str = original.Build()->ToString();

  ExprBuilder moved = Col("name");
  moved = std::move(original);

  EXPECT_NE(moved.Build(), nullptr);
  EXPECT_EQ(moved.Build()->ToString(), original_str);
}

TEST(ExprBuilderRuleOfFive, BoolConversion) {
  auto valid = Col("id");
  EXPECT_TRUE(static_cast<bool>(valid));
}

// =============================================================================
// Concept Validation Tests (Compile-time checks)
// =============================================================================

static_assert(LiteralValue<int>);
static_assert(LiteralValue<double>);
static_assert(LiteralValue<bool>);
static_assert(LiteralValue<std::string>);
static_assert(!LiteralValue<std::vector<int>>);
static_assert(BatchCallback<std::function<void(RecordBatch)>>);
static_assert(BatchCallbackWithControl<std::function<bool(RecordBatch)>>);
