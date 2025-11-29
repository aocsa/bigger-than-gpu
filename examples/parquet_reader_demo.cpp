// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file parquet_reader_demo.cpp
/// @brief Example usage of the Parquet reader library with Result error handling.
///
/// Compile: g++ -std=c++23 -I../src -o parquet_example parquet_reader_demo.cpp

#include <format>
#include <iostream>
#include <string>
#include <vector>

#include "quest/datasource/parquet_reader.hpp"

using namespace quest::datasource;

void PrintSchema(const Schema& schema) {
  std::cout << "Schema:\n";
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    const auto& field = schema.field(i);
    std::cout << std::format("  [{}] {} : {}\n", i, field.name,
                             GetTypeName(field.data_type));
  }
}

void PrintRecordBatch(const RecordBatch& batch, size_t max_rows = 10) {
  const auto& schema = batch.schema();

  // Print header.
  for (size_t col = 0; col < schema.num_fields(); ++col) {
    if (col > 0) {
      std::cout << " | ";
    }
    std::cout << std::format("{:>15}", schema.field(col).name);
  }
  std::cout << "\n" << std::string(schema.num_fields() * 18, '-') << "\n";

  // Print rows.
  size_t rows_to_print = std::min(batch.num_rows(), max_rows);
  for (size_t row = 0; row < rows_to_print; ++row) {
    for (size_t col = 0; col < batch.num_columns(); ++col) {
      if (col > 0) {
        std::cout << " | ";
      }
      std::string value = ValueToString(batch.column(col).GetValue(row));
      if (value.length() > 15) {
        value = value.substr(0, 12) + "...";
      }
      std::cout << std::format("{:>15}", value);
    }
    std::cout << "\n";
  }

  if (batch.num_rows() > max_rows) {
    std::cout << std::format("... and {} more rows\n",
                             batch.num_rows() - max_rows);
  }
}

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: " << argv[0] << " <parquet_file> [column1,column2,...]\n";
    return 1;
  }

  std::string filename = argv[1];
  std::vector<std::string> columns;

  if (argc >= 3) {
    // Parse column list.
    std::string col_list = argv[2];
    size_t start = 0;
    size_t pos = 0;
    while ((pos = col_list.find(',', start)) != std::string::npos) {
      columns.push_back(col_list.substr(start, pos - start));
      start = pos + 1;
    }
    columns.push_back(col_list.substr(start));
  }

  std::cout << "Opening: " << filename << "\n\n";

  // Open the Parquet file using Result-based error handling.
  auto parquet_result = ParquetDataSource::Open(filename);
  if (!parquet_result.has_value()) {
    std::cerr << "Error: " << parquet_result.error().message() << "\n";
    std::cerr << "Error code: " << static_cast<int>(parquet_result.error().code()) << "\n";
    std::cerr << "ToString: " << parquet_result.error().ToString() << "\n";
    return 1;
  }
  auto& parquet = parquet_result.value();

  // Print file info.
  std::cout << "File Statistics:\n";
  std::cout << std::format("  Total rows: {}\n", parquet.num_rows());
  std::cout << std::format("  Row groups: {}\n", parquet.num_row_groups());
  std::cout << "\n";

  // Print schema.
  PrintSchema(parquet.schema());
  std::cout << "\n";

  // Print metadata if available.
  const auto& metadata = parquet.metadata();
  if (metadata.created_by.has_value()) {
    std::cout << "Created by: " << *metadata.created_by << "\n";
  }
  if (!metadata.key_value_metadata.empty()) {
    std::cout << "Key-Value Metadata:\n";
    for (const auto& kv : metadata.key_value_metadata) {
      std::cout << std::format("  {}: {}\n", kv.key,
                               kv.value.value_or("<null>"));
    }
  }
  std::cout << "\n";

  // Scan and print data.
  std::cout << "Data:\n";
  size_t row_group_idx = 0;

  // Use explicit iterator since operator* returns Result<RecordBatch>.
  auto sequence = parquet.Scan(columns);
  for (auto it = sequence.begin(); it != sequence.end(); ++it) {
    auto batch_result = *it;
    if (!batch_result.has_value()) {
      std::cerr << "Error reading row group " << row_group_idx << ": "
                << batch_result.error().message() << "\n";
      return 1;
    }

    auto& batch = batch_result.value();
    std::cout << std::format("=== Row Group {} ({} rows) ===\n",
                             row_group_idx++, batch.num_rows());
    PrintRecordBatch(batch);
    std::cout << "\n";
  }

  std::cout << "Done.\n";
  return 0;
}
