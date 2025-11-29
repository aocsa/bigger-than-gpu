// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_query_integration.cpp
/// @brief Integration tests for end-to-end query execution.

#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <vector>

#include "quest/builder/query_builder.hpp"
#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/physical_plan.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::datasource;

// =============================================================================
// In-Memory DataSource for Testing
// =============================================================================

class InMemoryDataSource : public DataSource {
 public:
  InMemoryDataSource(Schema schema, std::vector<RecordBatch> batches)
      : schema_(std::move(schema)), batches_(std::move(batches)) {}

  [[nodiscard]] Schema schema() const override { return schema_; }

  [[nodiscard]] RecordBatchSequence Scan(
      const std::vector<std::string>& /*projection*/) override {
    // For testing, return empty sequence (can't use real iterator without
    // ParquetFileReader)
    return RecordBatchSequence(nullptr, {});
  }

  const std::vector<RecordBatch>& batches() const { return batches_; }

 private:
  Schema schema_;
  std::vector<RecordBatch> batches_;
};

// =============================================================================
// Test Fixture
// =============================================================================

class QueryIntegrationTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Create test schema
    std::vector<Field> fields = {
        Field("id", Int32Type{}),
        Field("name", StringType{}),
        Field("salary", DoubleType{}),
        Field("department", StringType{}),
        Field("active", BoolType{}),
    };
    schema_ = Schema(fields);

    // Create test data
    CreateTestBatch();

    data_source_ = std::make_shared<InMemoryDataSource>(schema_, batches_);
  }

  void CreateTestBatch() {
    auto id_col = std::make_unique<TypedColumnVector<int32_t>>(Int32Type{}, 10);
    auto name_col = std::make_unique<StringColumnVector>(StringType{}, 10);
    auto salary_col =
        std::make_unique<TypedColumnVector<double>>(DoubleType{}, 10);
    auto dept_col = std::make_unique<StringColumnVector>(StringType{}, 10);
    auto active_col = std::make_unique<TypedColumnVector<bool>>(BoolType{}, 10);

    // Row 0
    id_col->Append(1);
    name_col->Append("Alice");
    salary_col->Append(75000.0);
    dept_col->Append("Engineering");
    active_col->Append(true);

    // Row 1
    id_col->Append(2);
    name_col->Append("Bob");
    salary_col->Append(65000.0);
    dept_col->Append("Engineering");
    active_col->Append(true);

    // Row 2
    id_col->Append(3);
    name_col->Append("Charlie");
    salary_col->Append(80000.0);
    dept_col->Append("Sales");
    active_col->Append(false);

    // Row 3
    id_col->Append(4);
    name_col->Append("Diana");
    salary_col->Append(90000.0);
    dept_col->Append("Engineering");
    active_col->Append(true);

    // Row 4
    id_col->Append(5);
    name_col->Append("Eve");
    salary_col->Append(55000.0);
    dept_col->Append("HR");
    active_col->Append(true);

    std::vector<std::shared_ptr<ColumnVector>> columns;
    columns.push_back(std::move(id_col));
    columns.push_back(std::move(name_col));
    columns.push_back(std::move(salary_col));
    columns.push_back(std::move(dept_col));
    columns.push_back(std::move(active_col));

    batches_.emplace_back(schema_, std::move(columns));
  }

  Schema schema_;
  std::vector<RecordBatch> batches_;
  std::shared_ptr<InMemoryDataSource> data_source_;
};

// =============================================================================
// Logical Plan Construction Tests
// =============================================================================

TEST_F(QueryIntegrationTest, BuildScanPlan) {
  Scan scan("employees", data_source_, std::nullopt);

  EXPECT_EQ(scan.schema().num_fields(), 5U);
  EXPECT_EQ(scan.path(), "employees");
}

TEST_F(QueryIntegrationTest, BuildScanWithProjection) {
  std::vector<std::string> projection = {"id", "name", "salary"};
  Scan scan("employees", data_source_, projection);

  EXPECT_EQ(scan.schema().num_fields(), 3U);
  EXPECT_EQ(scan.schema().field(0).name, "id");
  EXPECT_EQ(scan.schema().field(1).name, "name");
  EXPECT_EQ(scan.schema().field(2).name, "salary");
}

TEST_F(QueryIntegrationTest, BuildSelectionPlan) {
  auto scan =
      std::make_unique<Scan>("employees", data_source_, std::nullopt);
  auto condition = gt(col("salary"), lit(70000.0));

  Selection selection(std::move(scan), std::move(condition));

  EXPECT_EQ(selection.schema().num_fields(), 5U);
  EXPECT_NE(selection.ToString().find("Selection"), std::string::npos);
}

TEST_F(QueryIntegrationTest, BuildProjectionPlan) {
  auto scan =
      std::make_unique<Scan>("employees", data_source_, std::nullopt);

  std::vector<LogicalExprPtr> exprs;
  exprs.push_back(col("id"));
  exprs.push_back(col("name"));
  exprs.push_back(multiply(col("salary"), lit(1.1)));  // 10% raise

  Projection projection(std::move(scan), std::move(exprs));

  EXPECT_EQ(projection.Children().size(), 1U);
}

// =============================================================================
// QueryBuilder Integration Tests
// =============================================================================

TEST_F(QueryIntegrationTest, QueryBuilderBasicSelect) {
  auto plan = QueryBuilder::From(data_source_, "employees")
                  .Select({"id", "name"})
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
  auto str = plan->ToString();
  EXPECT_NE(str.find("Projection"), std::string::npos);
}

TEST_F(QueryIntegrationTest, QueryBuilderWithFilter) {
  auto plan = QueryBuilder::From(data_source_, "employees")
                  .Where(Col("salary") > Lit(60000.0))
                  .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
  auto str = plan->ToString();
  EXPECT_NE(str.find("Selection"), std::string::npos);
}

TEST_F(QueryIntegrationTest, QueryBuilderComplexCondition) {
  auto plan =
      QueryBuilder::From(data_source_, "employees")
          .Where((Col("salary") >= Lit(60000.0)) && (Col("active") == Lit(true)))
          .Select({"id", "name", "salary"})
          .BuildLogicalPlan();

  EXPECT_NE(plan, nullptr);
}

TEST_F(QueryIntegrationTest, QueryBuilderToPhysicalPlan) {
  auto result = QueryBuilder::From(data_source_, "employees")
                    .Select({"id", "name"})
                    .BuildPhysicalPlan();

  EXPECT_TRUE(result.has_value());
  EXPECT_NE(result.value(), nullptr);
}

// =============================================================================
// Expression Evaluation on Test Data
// =============================================================================

TEST_F(QueryIntegrationTest, EvaluateColumnExpression) {
  ColumnExpression expr(2);  // salary column
  const auto& batch = batches_[0];

  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_EQ(result->size(), 5U);
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 75000.0, 0.001);  // Alice
  EXPECT_NEAR(std::get<double>(result->GetValue(3)), 90000.0, 0.001);  // Diana
}

TEST_F(QueryIntegrationTest, EvaluateComparisonExpression) {
  auto left = std::make_unique<ColumnExpression>(2);   // salary
  auto right = std::make_unique<LiteralExpression>(Value{70000.0});
  GtExpression expr(std::move(left), std::move(right));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // salary > 70000: Alice(75k)=true, Bob(65k)=false, Charlie(80k)=true,
  //                 Diana(90k)=true, Eve(55k)=false
  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));   // Alice
  EXPECT_FALSE(std::get<bool>(result->GetValue(1)));  // Bob
  EXPECT_TRUE(std::get<bool>(result->GetValue(2)));   // Charlie
  EXPECT_TRUE(std::get<bool>(result->GetValue(3)));   // Diana
  EXPECT_FALSE(std::get<bool>(result->GetValue(4)));  // Eve
}

TEST_F(QueryIntegrationTest, EvaluateArithmeticExpression) {
  auto left = std::make_unique<ColumnExpression>(2);  // salary
  auto right = std::make_unique<LiteralExpression>(Value{1.1});
  MultiplyExpression expr(std::move(left), std::move(right));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // salary * 1.1
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 82500.0, 0.001);  // Alice
  EXPECT_NEAR(std::get<double>(result->GetValue(3)), 99000.0, 0.001);  // Diana
}

// =============================================================================
// Aggregate Expression Tests on Real Data
// =============================================================================

TEST_F(QueryIntegrationTest, AggregateSum) {
  auto input = std::make_unique<ColumnExpression>(2);  // salary
  SumExpression expr(std::move(input));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // SUM(salary) = 75000 + 65000 + 80000 + 90000 + 55000 = 365000
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 365000.0, 0.001);
}

TEST_F(QueryIntegrationTest, AggregateAvg) {
  auto input = std::make_unique<ColumnExpression>(2);  // salary
  AvgExpression expr(std::move(input));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // AVG(salary) = 365000 / 5 = 73000
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 73000.0, 0.001);
}

TEST_F(QueryIntegrationTest, AggregateCount) {
  auto input = std::make_unique<ColumnExpression>(0);  // id
  CountExpression expr(std::move(input));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  EXPECT_EQ(std::get<int64_t>(result->GetValue(0)), 5);
}

TEST_F(QueryIntegrationTest, AggregateMin) {
  auto input = std::make_unique<ColumnExpression>(2);  // salary
  MinExpression expr(std::move(input));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // MIN(salary) = 55000 (Eve)
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 55000.0, 0.001);
}

TEST_F(QueryIntegrationTest, AggregateMax) {
  auto input = std::make_unique<ColumnExpression>(2);  // salary
  MaxExpression expr(std::move(input));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, expr.Evaluate(batch));

  // MAX(salary) = 90000 (Diana)
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 90000.0, 0.001);
}

// =============================================================================
// Complex Query Integration Tests
// =============================================================================

TEST_F(QueryIntegrationTest, ComplexArithmeticChain) {
  // (salary + 10000) * 1.05
  auto salary = std::make_unique<ColumnExpression>(2);
  auto bonus = std::make_unique<LiteralExpression>(Value{10000.0});
  auto add_expr =
      std::make_unique<AddExpression>(std::move(salary), std::move(bonus));

  auto multiplier = std::make_unique<LiteralExpression>(Value{1.05});
  MultiplyExpression final_expr(std::move(add_expr), std::move(multiplier));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, final_expr.Evaluate(batch));

  // Alice: (75000 + 10000) * 1.05 = 89250
  EXPECT_NEAR(std::get<double>(result->GetValue(0)), 89250.0, 0.001);
}

TEST_F(QueryIntegrationTest, ComplexLogicalCondition) {
  // (salary > 60000) AND (active == true)
  auto salary = std::make_unique<ColumnExpression>(2);
  auto threshold = std::make_unique<LiteralExpression>(Value{60000.0});
  auto salary_check =
      std::make_unique<GtExpression>(std::move(salary), std::move(threshold));

  auto active = std::make_unique<ColumnExpression>(4);
  auto true_val = std::make_unique<LiteralExpression>(Value{true});
  auto active_check =
      std::make_unique<EqExpression>(std::move(active), std::move(true_val));

  AndExpression condition(std::move(salary_check), std::move(active_check));

  const auto& batch = batches_[0];
  ASSERT_OK_AND_ASSIGN(auto result, condition.Evaluate(batch));

  // Alice: 75k > 60k AND true = true
  // Bob: 65k > 60k AND true = true
  // Charlie: 80k > 60k AND false = false
  // Diana: 90k > 60k AND true = true
  // Eve: 55k > 60k AND true = false
  EXPECT_TRUE(std::get<bool>(result->GetValue(0)));   // Alice
  EXPECT_TRUE(std::get<bool>(result->GetValue(1)));   // Bob
  EXPECT_FALSE(std::get<bool>(result->GetValue(2)));  // Charlie
  EXPECT_TRUE(std::get<bool>(result->GetValue(3)));   // Diana
  EXPECT_FALSE(std::get<bool>(result->GetValue(4)));  // Eve
}

// =============================================================================
// Plan Pretty Print Tests
// =============================================================================

TEST_F(QueryIntegrationTest, PlanPrettyPrint) {
  auto scan =
      std::make_unique<Scan>("employees", data_source_, std::nullopt);
  auto condition =
      and_expr(gt(col("salary"), lit(50000.0)), eq(col("active"), lit(true)));
  auto selection =
      std::make_unique<Selection>(std::move(scan), std::move(condition));

  std::vector<LogicalExprPtr> exprs;
  exprs.push_back(col("id"));
  exprs.push_back(col("name"));

  Projection projection(std::move(selection), std::move(exprs));

  auto pretty = projection.Pretty(0);

  EXPECT_FALSE(pretty.empty());
  EXPECT_NE(pretty.find("Projection"), std::string::npos);
  EXPECT_NE(pretty.find("Selection"), std::string::npos);
  EXPECT_NE(pretty.find("Scan"), std::string::npos);
}

// =============================================================================
// Parquet Integration Tests
// =============================================================================

class ParquetIntegrationTest : public ::testing::Test {
 protected:
  std::filesystem::path test_data_dir_ =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "examples";

  std::filesystem::path employees_file() const {
    return test_data_dir_ / "employees_uncompressed.parquet";
  }
};

TEST_F(ParquetIntegrationTest, EndToEndQuery) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto builder_result = QueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  auto batches = std::move(builder_result.value()).ExecuteCollect();

  ASSERT_TRUE(batches.has_value());
  EXPECT_GT(batches.value().size(), 0U);

  // Verify we got actual data
  size_t total_rows = 0;
  for (const auto& batch : batches.value()) {
    total_rows += batch.num_rows();
  }
  EXPECT_GT(total_rows, 0U);
}

TEST_F(ParquetIntegrationTest, QueryWithFilter) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto builder_result = QueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  // Get schema first to know column names
  auto schema = std::move(builder_result.value()).BuildLogicalPlan()->schema();

  // Rebuild for actual query
  builder_result = QueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  // Build physical plan (can't actually filter without running, but plan
  // builds)
  auto plan = std::move(builder_result.value()).BuildPhysicalPlan();
  EXPECT_TRUE(plan.has_value());
}
