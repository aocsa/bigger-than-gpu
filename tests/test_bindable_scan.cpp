// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_bindable_scan.cpp
/// @brief Phase 2.7 Stage F3: the gate folds Projection(Scan) with pure column
///        references into one BindableTableScan that reads fewer columns.

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include "quest/builder/expr_builder.hpp"
#include "quest/builder/query_builder.hpp"
#include "quest/execution/graph_builder.hpp"
#include "quest/taskflow/meta_pipeline.hpp"
#include "quest/taskflow/operators/table_scan_operator.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::taskflow;
using namespace quest::execution;
using namespace quest::datasource;
using namespace quest::testing;

namespace {

[[nodiscard]] std::filesystem::path EmployeesParquet() {
  return std::filesystem::path(__FILE__).parent_path().parent_path() / "examples" /
         "employees_uncompressed.parquet";
}

[[nodiscard]] std::size_t TotalRows(const std::vector<RecordBatch>& batches) {
  std::size_t total = 0;
  for (const auto& batch : batches) {
    total += batch.num_rows();
  }
  return total;
}

/// @brief Returns the pipeline whose source is a TableScanOperator (the scan
/// pipeline), or nullptr.
[[nodiscard]] const Pipeline* FindScanPipeline(const std::vector<PipelinePtr>& pipelines) {
  for (const auto& pipeline : pipelines) {
    if (dynamic_cast<const TableScanOperator*>(pipeline->source()) != nullptr) {
      return pipeline.get();
    }
  }
  return nullptr;
}

}  // namespace

// =============================================================================
// Fold shape
// =============================================================================

TEST(BindableScanTest, FoldsProjectionOnScanToBindableScan) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({"id"})
                  .BuildLogicalPlan();

  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  // One scan pipeline + one collector pipeline; the Project is gone.
  ASSERT_EQ(pipelines.size(), 2U);
  const Pipeline* scan_pipeline = FindScanPipeline(pipelines);
  ASSERT_NE(scan_pipeline, nullptr);
  ASSERT_EQ(scan_pipeline->operators().size(), 1U);
  EXPECT_EQ(scan_pipeline->operators().front()->type(), OperatorType::BindableTableScan);
}

TEST(BindableScanTest, ReadsFewerColumns) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  const auto path = EmployeesParquet().string();

  // Full scan tells us the source column count.
  ASSERT_OK_AND_ASSIGN(auto all_batches, QueryBuilder::FromParquet(path).value().ExecuteCollect());
  ASSERT_FALSE(all_batches.empty());
  const std::size_t source_columns = all_batches.front().num_columns();
  ASSERT_GT(source_columns, 1U);

  auto plan = QueryBuilder::FromParquet(path).value().Select({"id"}).BuildLogicalPlan();
  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  const Pipeline* scan_pipeline = FindScanPipeline(pipelines);
  ASSERT_NE(scan_pipeline, nullptr);
  const auto* scan = dynamic_cast<const TableScanOperator*>(scan_pipeline->source());
  ASSERT_NE(scan, nullptr);
  ASSERT_TRUE(scan->projection().has_value());
  EXPECT_EQ(scan->projection()->size(), 1U);
  EXPECT_EQ(scan->projection()->front(), "id");
  EXPECT_LT(scan->projection()->size(), source_columns);  // provably fewer columns
}

TEST(BindableScanTest, MultiColumnFoldPreservesOrder) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({"name", "id"})  // reordered relative to source
                  .BuildLogicalPlan();
  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  const Pipeline* scan_pipeline = FindScanPipeline(pipelines);
  ASSERT_NE(scan_pipeline, nullptr);
  const auto* scan = dynamic_cast<const TableScanOperator*>(scan_pipeline->source());
  ASSERT_NE(scan, nullptr);
  ASSERT_TRUE(scan->projection().has_value());
  ASSERT_EQ(scan->projection()->size(), 2U);
  EXPECT_EQ((*scan->projection())[0], "name");
  EXPECT_EQ((*scan->projection())[1], "id");
}

TEST(BindableScanTest, ArithmeticProjectionNotFolded) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  // A computed column is not a pure reference, so the fold must not apply.
  auto plan = QueryBuilder::FromParquet(EmployeesParquet().string())
                  .value()
                  .Select({Col("id"), Col("salary") + Lit(1000.0)})
                  .BuildLogicalPlan();
  ASSERT_OK_AND_ASSIGN(auto exec, CreateGraph(*plan));
  ASSERT_OK_AND_ASSIGN(auto pipelines, MetaPipeline::Build(exec->graph()));

  const Pipeline* scan_pipeline = FindScanPipeline(pipelines);
  ASSERT_NE(scan_pipeline, nullptr);
  // Two operators, and the scan stayed a plain TableScan (not folded).
  ASSERT_EQ(scan_pipeline->operators().size(), 2U);
  EXPECT_EQ(scan_pipeline->operators().front()->type(), OperatorType::TableScan);
  EXPECT_EQ(scan_pipeline->operators().back()->type(), OperatorType::Project);
}

// =============================================================================
// Correctness / parity
// =============================================================================

TEST(BindableScanTest, OutputSchemaIsProjected) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  ASSERT_OK_AND_ASSIGN(auto batches, QueryBuilder::FromParquet(EmployeesParquet().string())
                                         .value()
                                         .Select({"id"})
                                         .ExecuteOnGraph());
  ASSERT_FALSE(batches.empty());
  EXPECT_EQ(batches.front().num_columns(), 1U);
}

TEST(BindableScanTest, ParityWithUnfoldedCallback) {
  if (!std::filesystem::exists(EmployeesParquet())) {
    GTEST_SKIP() << "missing " << EmployeesParquet();
  }
  const auto path = EmployeesParquet().string();

  ASSERT_OK_AND_ASSIGN(auto graph_batches,
                       QueryBuilder::FromParquet(path).value().Select({"name", "id"}).ExecuteOnGraph());
  ASSERT_OK_AND_ASSIGN(auto callback_batches,
                       QueryBuilder::FromParquet(path).value().Select({"name", "id"}).ExecuteCollect());

  EXPECT_EQ(TotalRows(graph_batches), TotalRows(callback_batches));
  ASSERT_FALSE(graph_batches.empty());
  ASSERT_FALSE(callback_batches.empty());
  EXPECT_EQ(graph_batches.front().num_columns(), callback_batches.front().num_columns());
  EXPECT_EQ(graph_batches.front().num_columns(), 2U);
}
