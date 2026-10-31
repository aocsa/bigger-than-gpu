// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file simple_graph.cpp
/// @brief Run a filter-and-project query through the pipeline executor.

#include <iostream>
#include <string>
#include <utility>

#include "quest/builder/query_builder.hpp"

int main(int argc, char* argv[]) {
  constexpr double kMinimumSalary = 50'000.0;
  const std::string path = argc > 1 ? argv[1] : "examples/employees_uncompressed.parquet";

  auto builder = quest::QueryBuilder::FromParquet(path);
  if (!builder) {
    std::cerr << "Failed to open '" << path << "': " << builder.error().message_ << '\n';
    return 1;
  }

  auto result = std::move(*builder)
                    .Where(quest::Col("salary") > quest::Lit(kMinimumSalary))
                    .Select({"id", "name", "salary"})
                    .ExecuteOnGraph();
  if (!result) {
    std::cerr << "Pipeline execution failed: " << result.error().message_ << '\n';
    return 1;
  }

  for (const auto& batch : *result) {
    std::cout << batch.ToCSV();
  }
  return 0;
}
