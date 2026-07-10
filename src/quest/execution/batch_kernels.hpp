// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_EXECUTION_BATCH_KERNELS_HPP_
#define QUEST_EXECUTION_BATCH_KERNELS_HPP_

/// @file batch_kernels.hpp
/// @brief Shared RecordBatch filter/project helpers for physical + taskflow paths.

#include <algorithm>
#include <cstddef>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/physical_plan/expressions/physical_expr.hpp"
#include "quest/result.hpp"

namespace quest::execution {

/// @brief Gather selected rows from a column into a new materialized column.
[[nodiscard]] inline std::shared_ptr<datasource::ColumnVector> GatherColumn(
    const datasource::ColumnVector& col, const std::vector<size_t>& indices) {
  std::vector<Value> filtered_values;
  filtered_values.reserve(indices.size());
  for (size_t idx : indices) {
    filtered_values.push_back(col.GetValue(idx));
  }

  return std::visit(
      [&filtered_values](const auto& type_tag) -> std::shared_ptr<datasource::ColumnVector> {
        using TypeTag = std::decay_t<decltype(type_tag)>;

        if constexpr (std::is_same_v<TypeTag, datasource::BoolType>) {
          std::vector<bool> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<bool>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(false);
            }
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, datasource::Int32Type>) {
          std::vector<int32_t> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<int32_t>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0);
            }
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, datasource::Int64Type>) {
          std::vector<int64_t> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<int64_t>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0);
            }
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, datasource::FloatType>) {
          std::vector<float> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<float>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0.0f);
            }
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, datasource::DoubleType>) {
          std::vector<double> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<double>(&val)) {
              data.push_back(*v);
            } else {
              data.push_back(0.0);
            }
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)}, type_tag);
        } else if constexpr (std::is_same_v<TypeTag, datasource::StringType>) {
          std::vector<std::string> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            if (auto* v = std::get_if<std::string>(&val)) {
              data.push_back(*v);
            } else {
              data.emplace_back();
            }
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)}, type_tag);
        } else {
          std::vector<std::string> data;
          data.reserve(filtered_values.size());
          for (const auto& val : filtered_values) {
            data.push_back(datasource::ValueToString(val));
          }
          return std::make_shared<MaterializedColumnVector>(ColumnData{std::move(data)},
                                                            datasource::StringType{});
        }
      },
      col.GetType());
}

/// @brief Apply a boolean mask expression and gather matching rows.
[[nodiscard]] inline Result<datasource::RecordBatch> FilterRecordBatch(
    const datasource::RecordBatch& batch, Expression& expr) {
  auto mask_result = expr.Evaluate(batch);
  if (!mask_result) {
    return std::unexpected(mask_result.error());
  }

  auto& mask_col = **mask_result;
  std::vector<size_t> indices;
  indices.reserve(batch.num_rows());
  for (size_t i = 0; i < batch.num_rows(); ++i) {
    auto val = mask_col.GetValue(i);
    if (auto* bool_val = std::get_if<bool>(&val)) {
      if (*bool_val) {
        indices.push_back(i);
      }
    }
  }

  if (indices.empty()) {
    return datasource::RecordBatch(batch.schema(), {});
  }

  std::vector<std::shared_ptr<datasource::ColumnVector>> new_columns;
  new_columns.reserve(batch.num_columns());
  for (size_t col_idx = 0; col_idx < batch.num_columns(); ++col_idx) {
    new_columns.push_back(GatherColumn(batch.column(col_idx), indices));
  }
  return datasource::RecordBatch(batch.schema(), std::move(new_columns));
}

/// @brief Truncate a RecordBatch to its first @p n_rows rows via GatherColumn.
///
/// If @p n_rows is 0, returns an empty batch with the same schema. If
/// @p n_rows >= batch.num_rows(), gathers every row (a full copy).
[[nodiscard]] inline Result<datasource::RecordBatch> TruncateRecordBatch(
    const datasource::RecordBatch& batch, std::size_t n_rows) {
  if (n_rows == 0) {
    return datasource::RecordBatch(batch.schema(), {});
  }
  const std::size_t take = std::min(n_rows, batch.num_rows());
  std::vector<size_t> indices(take);
  for (std::size_t i = 0; i < take; ++i) {
    indices[i] = i;
  }

  std::vector<std::shared_ptr<datasource::ColumnVector>> new_columns;
  new_columns.reserve(batch.num_columns());
  for (size_t col_idx = 0; col_idx < batch.num_columns(); ++col_idx) {
    new_columns.push_back(GatherColumn(batch.column(col_idx), indices));
  }
  return datasource::RecordBatch(batch.schema(), std::move(new_columns));
}

/// @brief Evaluate projection expressions into a new RecordBatch.
[[nodiscard]] inline Result<datasource::RecordBatch> ProjectRecordBatch(
    const datasource::RecordBatch& batch, const datasource::Schema& schema,
    const std::vector<ExpressionPtr>& expressions) {
  std::vector<std::shared_ptr<datasource::ColumnVector>> columns;
  columns.reserve(expressions.size());
  for (const auto& expr : expressions) {
    auto result = expr->Evaluate(batch);
    if (!result) {
      return std::unexpected(result.error());
    }
    columns.push_back(std::move(*result));
  }
  if (columns.size() != expressions.size()) {
    return Status::ExpressionError("Projection produced fewer columns than expressions");
  }
  return datasource::RecordBatch(schema, std::move(columns));
}

/// @brief Ensure a DataBlock is resident in CPU and return its HostMemoryBlock.
[[nodiscard]] inline Result<std::shared_ptr<cache::HostMemoryBlock>> EnsureHostBlock(
    cache::DataBlockPtr input) {
  if (!input) {
    return Status::Invalid("Null data block");
  }
  if (input->tier() == memory::MemoryTier::CPU) {
    auto host = std::dynamic_pointer_cast<cache::HostMemoryBlock>(input);
    if (!host) {
      return Status::Invalid("CPU-tier block is not a HostMemoryBlock");
    }
    return host;
  }
  return input->ToCpu();
}

/// @brief Wrap a RecordBatch in a HostMemoryBlock carrying both memory
/// resources from @p memory_manager (which may be null).
///
/// Every operator that creates output blocks must use this instead of a
/// bare HostMemoryBlock: without the CPU resource the allocation is
/// invisible to saturation accounting, and without the disk resource
/// ToDisk() fails — MaybeSpill then silently keeps the data in RAM, so a
/// plan with any filter/project before a pipeline break could never spill.
[[nodiscard]] inline std::shared_ptr<cache::HostMemoryBlock> MakeTrackedHostBlock(
    std::string block_id, datasource::RecordBatch batch,
    const std::shared_ptr<memory::MemoryResourceManager>& memory_manager) {
  std::shared_ptr<memory::CPUMemoryResource> cpu;
  std::shared_ptr<memory::DiskMemoryResource> disk;
  if (memory_manager) {
    cpu = memory_manager->cpu_ptr();
    disk = memory_manager->disk_ptr();
  }
  auto block = std::make_shared<cache::HostMemoryBlock>(std::move(block_id), std::move(batch),
                                                        std::move(cpu));
  if (disk) {
    block->set_disk_resource(std::move(disk));
  }
  return block;
}

/// @brief Append values from @p src onto a typed destination column.
[[nodiscard]] inline Status AppendColumnValues(datasource::ColumnVector& dest,
                                               const datasource::ColumnVector& src) {
  return std::visit(
      [&dest, &src](const auto& type_tag) -> Status {
        using TypeTag = std::decay_t<decltype(type_tag)>;
        auto append_one = [&](auto* typed) -> Status {
          if (!typed) {
            return Status::Invalid("Concatenate destination type mismatch");
          }
          for (size_t i = 0; i < src.size(); ++i) {
            auto val = src.GetValue(i);
            if (std::holds_alternative<std::monostate>(val)) {
              typed->AppendNull();
              continue;
            }
            if constexpr (std::is_same_v<TypeTag, datasource::BoolType>) {
              if (auto* v = std::get_if<bool>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (bool)");
              }
            } else if constexpr (std::is_same_v<TypeTag, datasource::Int8Type>) {
              if (auto* v = std::get_if<int8_t>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (int8)");
              }
            } else if constexpr (std::is_same_v<TypeTag, datasource::Int16Type>) {
              if (auto* v = std::get_if<int16_t>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (int16)");
              }
            } else if constexpr (std::is_same_v<TypeTag, datasource::Int32Type>) {
              if (auto* v = std::get_if<int32_t>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (int32)");
              }
            } else if constexpr (std::is_same_v<TypeTag, datasource::Int64Type>) {
              if (auto* v = std::get_if<int64_t>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (int64)");
              }
            } else if constexpr (std::is_same_v<TypeTag, datasource::FloatType>) {
              if (auto* v = std::get_if<float>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (float)");
              }
            } else if constexpr (std::is_same_v<TypeTag, datasource::DoubleType>) {
              if (auto* v = std::get_if<double>(&val)) {
                typed->Append(*v);
              } else {
                return Status::Invalid("Concatenate value type mismatch (double)");
              }
            } else {
              if (auto* v = std::get_if<std::string>(&val)) {
                typed->Append(*v);
              } else {
                typed->Append(datasource::ValueToString(val));
              }
            }
          }
          return Status::OK();
        };

        if constexpr (std::is_same_v<TypeTag, datasource::BoolType>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<bool>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, datasource::Int8Type>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<int8_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, datasource::Int16Type>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<int16_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, datasource::Int32Type>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<int32_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, datasource::Int64Type>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<int64_t>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, datasource::FloatType>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<float>*>(&dest));
        } else if constexpr (std::is_same_v<TypeTag, datasource::DoubleType>) {
          return append_one(dynamic_cast<datasource::TypedColumnVector<double>*>(&dest));
        } else {
          return append_one(dynamic_cast<datasource::StringColumnVector*>(&dest));
        }
      },
      src.GetType());
}

/// @brief Column-wise concatenate of HostMemoryBlock RecordBatches.
[[nodiscard]] inline Result<cache::DataBlockPtr> ConcatenateHostBlocks(
    std::vector<cache::DataBlockPtr> blocks) {
  if (blocks.empty()) {
    return Status::Invalid("No data blocks to concatenate");
  }
  if (blocks.size() == 1) {
    return std::move(blocks[0]);
  }

  std::vector<std::shared_ptr<cache::HostMemoryBlock>> hosts;
  hosts.reserve(blocks.size());
  size_t total_rows = 0;
  for (auto& block : blocks) {
    auto host_result = EnsureHostBlock(block);
    if (!host_result) {
      return std::unexpected(host_result.error());
    }
    auto host = std::move(*host_result);
    if (!host->record_batch()) {
      return Status::Invalid("HostMemoryBlock has no record batch");
    }
    total_rows += host->num_rows();
    hosts.push_back(std::move(host));
  }

  const auto& schema = hosts[0]->record_batch()->schema();
  const size_t num_cols = schema.num_fields();
  for (size_t i = 1; i < hosts.size(); ++i) {
    if (hosts[i]->record_batch()->schema().num_fields() != num_cols) {
      return Status::SchemaError("Concatenate schema mismatch");
    }
  }

  std::vector<std::shared_ptr<datasource::ColumnVector>> columns;
  columns.reserve(num_cols);
  for (size_t c = 0; c < num_cols; ++c) {
    auto unique_col = datasource::CreateColumnVector(schema.field(c).data_type, total_rows);
    std::shared_ptr<datasource::ColumnVector> dest(std::move(unique_col));
    for (const auto& host : hosts) {
      auto status = AppendColumnValues(*dest, host->record_batch()->column(c));
      if (!status) {
        return std::unexpected(status.error());
      }
    }
    columns.push_back(std::move(dest));
  }

  datasource::RecordBatch merged(schema, std::move(columns));
  return std::make_shared<cache::HostMemoryBlock>("concatenated", std::move(merged));
}

}  // namespace quest::execution

#endif  // QUEST_EXECUTION_BATCH_KERNELS_HPP_
