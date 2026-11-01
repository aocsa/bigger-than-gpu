// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <string>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
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
//     - Type: TypeId, precision, scale (uint8_t each)
//     - Repetition (uint8_t)
// - Column data section (version 2):
//   - For each column: null count (uint64_t), then three buffers, each a byte length
//     (uint64_t) and the bytes as the column holds them: validity bitmap (empty when no
//     row is null), values (String: the chars), offsets (String only: rows + 1 int64).

class RecordBatchSerializer {
 public:
  [[nodiscard]] static Result<std::vector<uint8_t>> Serialize(const RecordBatch& batch);

  [[nodiscard]] static Result<RecordBatch> Deserialize(const std::vector<uint8_t>& buffer);

  [[nodiscard]] static Status SerializeToFile(const RecordBatch& batch,
                                              const std::string& filename);

  [[nodiscard]] static Result<RecordBatch> DeserializeFromFile(const std::string& filename);

 private:
  static Status WriteColumn(std::vector<uint8_t>& buffer, const ColumnVector& column, size_t num_rows);


  static Result<ColumnVectorPtr> ReadColumn(const std::vector<uint8_t>& buffer, size_t& offset,
                                            DataType type, size_t num_rows);
};

}  // namespace quest::columnar
