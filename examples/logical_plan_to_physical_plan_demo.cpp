// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file logical_plan_to_physical_plan_demo.cpp
/// @brief Demonstrates the query engine with logical and physical plans.
///
/// This demo shows how to:
/// 1. Build queries using the fluent QueryBuilder DSL
/// 2. Build queries manually using logical plan nodes
/// 3. Convert logical plans to physical plans
/// 4. Execute queries on Parquet files

#include <iostream>

#include "quest/builder/query_builder.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/logical_plan/logical_expr.hpp"
#include "quest/logical_plan/logical_plan.hpp"
#include "quest/physical_plan/physical_plan.hpp"

using namespace quest;

int main(int argc, char* argv[]) {
  // Default to test file in examples directory
  std::string filename = "/Users/alexander/git/bigger-than-gpu/examples/employees_uncompressed.parquet";
  if (argc > 1) {
    filename = argv[1];
  }

  std::cout << "=== Quest Query Engine Demo ===\n";
  std::cout << "Reading from: " << filename << "\n\n";

  // =========================================================================
  // Method 1: Using the QueryBuilder DSL
  // =========================================================================
  std::cout << "=== Method 1: Using QueryBuilder DSL ===\n";

  auto query_result = QueryBuilder::FromParquet(filename);
  if (!query_result.has_value()) {
    std::cerr << "Error opening file: " << query_result.error().message()
              << "\n";
    return 1;
  }

  // Build a query: SELECT id, name, salary FROM employees WHERE salary > 200000
  auto query = std::move(query_result.value())
                   .Where(Col("salary") > Lit(200000))
                   .Select({"id", "name", "salary"});

  std::cout << "Logical Plan:\n" << query.ToString() << "\n";

  // Execute the query
  int batch_num = 0;
  auto status = std::move(query).Execute([&batch_num](RecordBatch batch) {
    std::cout << "Batch " << batch_num++ << ":\n";
    std::cout << batch.ToString() << "\n";
  });

  if (!status.ok()) {
    std::cerr << "Execution error: " << status.message() << "\n";
    return 1;
  }

  // =========================================================================
  // Method 2: Building the logical plan manually
  // =========================================================================
  std::cout << "\n=== Method 2: Manual Logical Plan Construction ===\n";

  // Open the Parquet data source
  auto source_result = datasource::ParquetDataSource::Open(filename);
  if (!source_result.has_value()) {
    std::cerr << "Error opening file: " << source_result.error().message()
              << "\n";
    return 1;
  }

  auto source = std::make_shared<datasource::ParquetDataSource>(
      std::move(source_result.value()));

  // Build: SELECT name, tax FROM employees WHERE id < 5
  auto scan = std::make_unique<Scan>("employees", source, std::nullopt);

  // Create filter: id < 5
  auto filter_expr = lt(col("id"), lit(5));
  auto selection =
      std::make_unique<Selection>(std::move(scan), std::move(filter_expr));

  // Create projection: name, tax
  std::vector<LogicalExprPtr> projection_cols;
  projection_cols.push_back(col("name"));
  projection_cols.push_back(col("tax"));

  auto query_plan =
      std::make_unique<Projection>(std::move(selection), std::move(projection_cols));

  std::cout << "Logical Plan:\n" << query_plan->Pretty() << "\n";

  // Convert to physical plan
  auto physical_result = CreatePhysicalPlan(*query_plan);
  if (!physical_result.has_value()) {
    std::cerr << "Error creating physical plan: "
              << physical_result.error().message() << "\n";
    return 1;
  }

  auto& physical_plan = physical_result.value();
  std::cout << "Physical Plan:\n" << physical_plan->Pretty() << "\n";

  // Execute
  batch_num = 0;
  status = physical_plan->Execute([&batch_num](RecordBatch batch) {
    std::cout << "Batch " << batch_num++ << ":\n";
    std::cout << batch.ToString() << "\n";
  });

  if (!status.ok()) {
    std::cerr << "Execution error: " << status.message() << "\n";
    return 1;
  }

  // =========================================================================
  // Method 3: Complex query with OR conditions
  // =========================================================================
  std::cout << "\n=== Method 3: Complex Query with OR ===\n";

  auto complex_result = QueryBuilder::FromParquet(filename);
  if (!complex_result.has_value()) {
    std::cerr << "Error: " << complex_result.error().message() << "\n";
    return 1;
  }

  // SELECT id, name, salary FROM employees WHERE salary > 200000 OR id == 1
  auto complex_query =
      std::move(complex_result.value())
          .Where((Col("salary") > Lit(200000)) || (Col("id") == Lit(1)))
          .Select({"id", "name", "salary"});

  std::cout << "Logical Plan:\n" << complex_query.ToString() << "\n";

  batch_num = 0;
  status = std::move(complex_query).Execute([&batch_num](RecordBatch batch) {
    std::cout << "Batch " << batch_num++ << ":\n";
    std::cout << batch.ToString() << "\n";
  });

  if (!status.ok()) {
    std::cerr << "Execution error: " << status.message() << "\n";
    return 1;
  }

  std::cout << "\n=== Demo Complete ===\n";
  return 0;
}
