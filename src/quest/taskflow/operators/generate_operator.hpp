// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#pragma once

/// @file generate_operator.hpp
/// @brief Synthetic data source for dev/test pipelines.
///
/// Emits a configurable number of batches of int32 columns filled with a
/// deterministic sequence. Plays the source role (GetNextBatch), so it can seed
/// a real pipeline without a Parquet file — a first-class replacement for the
/// ad-hoc VectorSource doubles scattered across the tests.

#include <algorithm>
#include <cstdint>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

/// @brief Configurable synthetic source: @p batch_count batches of
/// @p rows_per_batch rows across @p num_columns int32 columns.
///
/// Column @c c of global row @c r holds `r * num_columns + c`, so every value
/// is distinct and rows are recoverable — handy for asserting a pipeline moved
/// exactly the rows it should.
class GenerateOperator : public Operator {
 public:
  GenerateOperator(std::uint32_t operator_id, std::size_t batch_count, std::size_t rows_per_batch,
                   std::size_t num_columns = 1,
                   std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr)
      : Operator(operator_id, "Generate", OperatorType::Generate),
        batch_count_(batch_count),
        rows_per_batch_(rows_per_batch),
        num_columns_(std::max<std::size_t>(1, num_columns)),
        memory_manager_(std::move(memory_manager)),
        schema_(BuildSchema(num_columns_)) {}

  [[nodiscard]] bool IsSource() const noexcept override { return true; }

  [[nodiscard]] const columnar::Schema& schema() const noexcept { return schema_; }

  [[nodiscard]] Result<std::optional<cache::DataBlockPtr>> GetNextBatch() override {
    if (emitted_ >= batch_count_) {
      return std::optional<cache::DataBlockPtr>{std::nullopt};
    }

    std::vector<std::shared_ptr<columnar::ColumnVector>> columns;
    columns.reserve(num_columns_);
    for (std::size_t col_idx = 0; col_idx < num_columns_; ++col_idx) {
      auto column = std::make_shared<columnar::TypedColumnVector<std::int32_t>>(
          columnar::DataType::kInt32, rows_per_batch_);
      for (std::size_t row = 0; row < rows_per_batch_; ++row) {
        column->Append(static_cast<std::int32_t>(((next_row_ + row) * num_columns_) + col_idx));
      }
      columns.push_back(std::move(column));
    }

    next_row_ += rows_per_batch_;
    ++emitted_;
    return std::optional<cache::DataBlockPtr>{cache::MakeTrackedHostBlock(
        std::format("generate_{}_{}", id(), emitted_),
        columnar::RecordBatch(schema_, std::move(columns)), memory_manager_)};
  }

 private:
  [[nodiscard]] static columnar::Schema BuildSchema(std::size_t num_columns) {
    std::vector<columnar::Field> fields;
    fields.reserve(num_columns);
    for (std::size_t col_idx = 0; col_idx < num_columns; ++col_idx) {
      fields.emplace_back("col" + std::to_string(col_idx), columnar::DataType::kInt32);
    }
    return columnar::Schema(std::move(fields));
  }

  std::size_t batch_count_;
  std::size_t rows_per_batch_;
  std::size_t num_columns_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;
  columnar::Schema schema_;
  std::size_t emitted_{0};
  std::size_t next_row_{0};
};

}  // namespace quest::taskflow
