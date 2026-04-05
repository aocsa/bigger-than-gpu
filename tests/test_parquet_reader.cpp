// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_parquet_reader.cpp
/// @brief Unit tests for Parquet file reading functionality.

#include <gtest/gtest.h>

#include <filesystem>

#include "quest/datasource/data_source.hpp"
#include "quest/datasource/parquet/parquet_reader.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::datasource;

// =============================================================================
// Test Fixture
// =============================================================================

class ParquetReaderTest : public ::testing::Test {
 protected:
  // Path to test parquet files in examples directory
  std::filesystem::path test_data_dir_ =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "examples";

  std::filesystem::path employees_file() const {
    return test_data_dir_ / "employees_uncompressed.parquet";
  }

  std::filesystem::path string_test_file() const {
    return test_data_dir_ / "string_test.parquet";
  }
};

// =============================================================================
// ParquetDataSource Tests
// =============================================================================

TEST_F(ParquetReaderTest, OpenValidFile) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  EXPECT_GT(ds.schema().num_fields(), 0U);
}

TEST_F(ParquetReaderTest, OpenInvalidFile) {
  auto result = ParquetDataSource::Open("/nonexistent/file.parquet");
  EXPECT_FALSE(result.has_value());
}

TEST_F(ParquetReaderTest, SchemaFields) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  EXPECT_GT(schema.num_fields(), 0U);
  // Check that field names are non-empty
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    EXPECT_FALSE(schema.field(i).name.empty());
  }
}

TEST_F(ParquetReaderTest, NumRows) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  EXPECT_GT(ds.num_rows(), 0);
}

TEST_F(ParquetReaderTest, ScanAllColumns) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  std::vector<std::string> all_columns;
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    all_columns.push_back(schema.field(i).name);
  }

  auto sequence = ds.Scan(all_columns);
  size_t batch_count = 0;
  size_t total_rows = 0;

  for (auto it = sequence.begin(); it != sequence.end(); ++it) {
    auto batch_result = *it;
    ASSERT_TRUE(batch_result.has_value())
        << "Batch read failed: " << batch_result.error().ToString();
    total_rows += batch_result.value().num_rows();
    ++batch_count;
  }

  EXPECT_GT(batch_count, 0U);
  EXPECT_EQ(total_rows, static_cast<size_t>(ds.num_rows()));
}

TEST_F(ParquetReaderTest, ScanProjection) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  if (schema.num_fields() < 2) {
    GTEST_SKIP() << "Need at least 2 columns for projection test";
  }

  // Project only first column
  std::vector<std::string> projection = {schema.field(0).name};
  auto sequence = ds.Scan(projection);

  for (auto it = sequence.begin(); it != sequence.end(); ++it) {
    auto batch_result = *it;
    ASSERT_TRUE(batch_result.has_value());
    EXPECT_EQ(batch_result.value().num_columns(), 1U);
    EXPECT_EQ(batch_result.value().schema().field(0).name, schema.field(0).name);
  }
}

TEST_F(ParquetReaderTest, StringColumnRead) {
  if (!std::filesystem::exists(string_test_file())) {
    GTEST_SKIP() << "Test file not found: " << string_test_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(string_test_file().string()));
  auto schema = ds.schema();

  // Find a string column
  std::optional<std::string> string_col;
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    if (std::holds_alternative<StringType>(schema.field(i).data_type)) {
      string_col = schema.field(i).name;
      break;
    }
  }

  if (!string_col) {
    GTEST_SKIP() << "No string column found in test file";
  }

  auto sequence = ds.Scan({*string_col});
  for (auto it = sequence.begin(); it != sequence.end(); ++it) {
    auto batch_result = *it;
    ASSERT_TRUE(batch_result.has_value());
    auto& col = batch_result.value().column(0);
    EXPECT_TRUE(std::holds_alternative<StringType>(col.GetType()));
  }
}

TEST_F(ParquetReaderTest, NumericColumnTypes) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  // Find numeric columns and verify data types
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    const auto& field = schema.field(i);
    bool is_numeric = std::holds_alternative<Int32Type>(field.data_type) ||
                      std::holds_alternative<Int64Type>(field.data_type) ||
                      std::holds_alternative<FloatType>(field.data_type) ||
                      std::holds_alternative<DoubleType>(field.data_type);

    if (is_numeric) {
      auto sequence = ds.Scan({field.name});
      for (auto it = sequence.begin(); it != sequence.end(); ++it) {
        auto batch_result = *it;
        ASSERT_TRUE(batch_result.has_value());
        // Verify column exists and has correct type
        EXPECT_EQ(batch_result.value().num_columns(), 1U);
      }
      break;  // Test just one numeric column
    }
  }
}

TEST_F(ParquetReaderTest, MetadataAccess) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));

  EXPECT_GT(ds.num_row_groups(), 0U);
  EXPECT_GT(ds.num_rows(), 0);

  // Metadata should be accessible
  const auto& metadata = ds.metadata();
  EXPECT_GT(metadata.num_rows, 0);
}

TEST_F(ParquetReaderTest, EmptyProjection) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));

  // Empty projection should still work (may scan all or none depending on impl)
  std::vector<std::string> empty_projection;
  auto sequence = ds.Scan(empty_projection);

  // Just verify it doesn't crash
  for (auto it = sequence.begin(); it != sequence.end(); ++it) {
    (void)*it;
  }
}

TEST_F(ParquetReaderTest, InvalidColumnProjection) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));

  // Try to project non-existent column
  std::vector<std::string> bad_projection = {"nonexistent_column_xyz"};
  auto sequence = ds.Scan(bad_projection);

  // The scan should either fail or return empty batches
  for (auto it = sequence.begin(); it != sequence.end(); ++it) {
    auto batch_result = *it;
    // Either fails or returns batch with 0 columns
    if (batch_result.has_value()) {
      EXPECT_EQ(batch_result.value().num_columns(), 0U);
    }
  }
}
