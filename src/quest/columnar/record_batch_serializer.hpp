// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <memory>
#include <string>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "quest/common/result.hpp"

namespace quest::columnar {

// Binary format serializer for RecordBatch
// Format:
// - Magic number (4 bytes): "QRBT" (Quest Record Batch)
// - Version (uint32_t): format version
// - Number of rows (uint64_t)
// - Number of columns (uint32_t)
// - Schema section:
//   - For each field:
//     - Name length (uint32_t)
//     - Name (variable length string)
//     - Type tag (uint8_t) - the DataType enum value
//     - Repetition (uint8_t)
// - Column data section:
//   - For each column:
//     - Null bitmap size (uint32_t)
//     - Null bitmap (variable length)
//     - Data (format depends on type)

class RecordBatchSerializer {
 public:
  [[nodiscard]] static Result<std::vector<uint8_t>> Serialize(const RecordBatch& batch);

  [[nodiscard]] static Result<RecordBatch> Deserialize(const std::vector<uint8_t>& buffer);

  [[nodiscard]] static Status SerializeToFile(const RecordBatch& batch,
                                              const std::string& filename);

  [[nodiscard]] static Result<RecordBatch> DeserializeFromFile(const std::string& filename);

 private:
  static void WriteDataType(std::vector<uint8_t>& buffer, DataType type);

  static Result<DataType> ReadDataType(const std::vector<uint8_t>& buffer, size_t& offset);

  static Status WriteColumn(std::vector<uint8_t>& buffer, const ColumnVector& column, size_t num_rows);

  static Status WriteColumnValues(std::vector<uint8_t>& buffer, const ColumnVector& column, size_t num_rows);

  static Result<std::shared_ptr<ColumnVector>> ReadColumn(const std::vector<uint8_t>& buffer,
                                                           size_t& offset,
                                                           DataType type,
                                                           size_t num_rows);

  static Status ReadColumnValues(const std::vector<uint8_t>& buffer,
                                size_t& offset,
                                ColumnVector& column,
                                DataType type,
                                size_t num_rows,
                                const std::vector<uint8_t>& null_bitmap);
};

}  // namespace quest::columnar
