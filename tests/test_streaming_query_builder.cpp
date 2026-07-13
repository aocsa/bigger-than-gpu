// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_streaming_query_builder.cpp
/// @brief Unit tests for StreamingQueryBuilder (coroutine/backpressure execution).

#include <gtest/gtest.h>

#include <filesystem>
#include <vector>

#include "quest/builder/streaming_query_builder.hpp"
#include "quest/datasource/data_source.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::datasource;

// =============================================================================
// StreamingQueryBuilder with Parquet Files
// =============================================================================

class StreamingQueryBuilderParquetTest : public ::testing::Test {
 protected:
  std::filesystem::path test_data_dir_ =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "examples";

  std::filesystem::path employees_file() const {
    return test_data_dir_ / "employees_uncompressed.parquet";
  }
};

TEST_F(StreamingQueryBuilderParquetTest, ExecuteStream) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto builder_result =
      StreamingQueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  std::vector<RecordBatch> batches;
  for (auto batch_result : std::move(builder_result.value()).ExecuteStream()) {
    if (batch_result.has_value()) {
      batches.push_back(std::move(*batch_result));
    }
  }

  EXPECT_GT(batches.size(), 0U);
}

TEST_F(StreamingQueryBuilderParquetTest, ExecuteStreamNextMethod) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  auto builder_result =
      StreamingQueryBuilder::FromParquet(employees_file().string());
  ASSERT_TRUE(builder_result.has_value());

  auto stream = std::move(builder_result.value()).ExecuteStream();

  // Use next() method for manual control
  size_t batch_count = 0;
  while (auto batch_opt = stream.next()) {
    ++batch_count;
    auto& batch_result = *batch_opt;
    EXPECT_TRUE(batch_result.has_value());
    if (batch_result.has_value()) {
      EXPECT_GT(batch_result->num_rows(), 0U);
    }
  }

  EXPECT_GT(batch_count, 0U);
}
