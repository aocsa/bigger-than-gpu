// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

/// @file metadata.hpp
/// @brief Parquet file metadata structures and Thrift footer parser.

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "quest/io/parquet/format.hpp"
#include "quest/common/result.hpp"

namespace quest::io::parquet {

using columnar::ColumnStatistics;
using columnar::DataType;
using columnar::Field;
using columnar::Repetition;
using columnar::Schema;

class ThriftDecoder;

// =============================================================================
// Metadata Structures
// =============================================================================

struct SchemaElement {
  PhysicalType type = PhysicalType::kBoolean;
  std::optional<int32_t> type_length;  ///< For fixed length byte arrays.
  Repetition repetition = Repetition::kRequired;
  std::string name;
  int32_t num_children = 0;
  std::optional<LogicalType> logical_type;
  std::optional<int32_t> precision;
  std::optional<int32_t> scale;
  std::optional<int32_t> field_id;
  std::optional<TimeUnit> time_unit;  ///< TIMESTAMP columns.
  std::optional<int32_t> bit_width;   ///< Integer annotation (IntType or INT_* / UINT_*).
  std::optional<bool> is_signed;      ///< Integer annotation (IntType or INT_* / UINT_*).

  /// @brief Returns true if this element is a leaf (has no children).
  [[nodiscard]] bool IsLeaf() const { return num_children == 0; }
};

struct ColumnChunkMetadata {
  std::string file_path;  ///< Optional, for external files.
  int64_t file_offset = 0;
  std::vector<std::string> column_path;
  PhysicalType type = PhysicalType::kBoolean;
  std::vector<Encoding> encodings;
  std::vector<std::string> path_in_schema;
  CompressionType codec = CompressionType::kUncompressed;
  int64_t num_values = 0;
  int64_t total_uncompressed_size = 0;
  int64_t total_compressed_size = 0;
  int64_t data_page_offset = 0;
  std::optional<int64_t> index_page_offset;
  std::optional<int64_t> dictionary_page_offset;
  ColumnStatistics statistics;
};

struct RowGroupMetadata {
  std::vector<ColumnChunkMetadata> columns;
  int64_t total_byte_size = 0;
  int64_t num_rows = 0;
  std::optional<int64_t> file_offset;
  std::optional<int64_t> total_compressed_size;
  std::optional<int16_t> ordinal;
};

struct KeyValue {
  std::string key;
  std::optional<std::string> value;
};

struct FileMetadata {
  int32_t version = 0;
  std::vector<SchemaElement> schema_elements;
  int64_t num_rows = 0;
  std::vector<RowGroupMetadata> row_groups;
  std::vector<KeyValue> key_value_metadata;
  std::optional<std::string> created_by;
};

// =============================================================================
// Metadata Parser
// =============================================================================

/// @brief Parses Thrift-encoded Parquet file footer metadata.
class MetadataParser {
 public:
  [[nodiscard]] static Result<FileMetadata> Parse(
      std::span<const uint8_t> data);

 private:
  // ===========================================================================
  // Top-Level Parsers
  // ===========================================================================

  [[nodiscard]] static Result<FileMetadata> ParseFileMetadata(
      ThriftDecoder& decoder);

  // ===========================================================================
  // Schema Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<SchemaElement>> ParseSchemaElements(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<SchemaElement> ParseSchemaElement(
      ThriftDecoder& decoder);

  // ===========================================================================
  // Row Group Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<RowGroupMetadata>> ParseRowGroups(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<RowGroupMetadata> ParseRowGroup(
      ThriftDecoder& decoder);

  // ===========================================================================
  // Column Chunk Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<ColumnChunkMetadata>> ParseColumnChunks(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<ColumnChunkMetadata> ParseColumnChunk(
      ThriftDecoder& decoder);

  [[nodiscard]] static Status ParseColumnMetadata(ThriftDecoder& decoder,
                                                   ColumnChunkMetadata& cc);

  // ===========================================================================
  // Helper Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<Encoding>> ParseEncodings(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<std::vector<std::string>> ParseStringList(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<ColumnStatistics> ParseStatistics(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<std::vector<KeyValue>> ParseKeyValueMetadata(
      ThriftDecoder& decoder);

  [[nodiscard]] static Result<KeyValue> ParseKeyValue(ThriftDecoder& decoder);
};

// =============================================================================
// Schema Building
// =============================================================================

/// @brief How the reader turns a leaf column's physical values into its DataType.
struct LeafType {
  enum class Conversion : uint8_t {
    kNone,       ///< The physical values are the column's values.
    kDecimal,    ///< INT32/INT64 DECIMAL to Decimal: the unscaled values widen to int64.
    kTimestamp,  ///< INT64 TIMESTAMP in `unit` to Timestamp micros.
    kInt96,      ///< Legacy INT96 timestamp (Julian day, nanos of day) to Timestamp.
    kUInt32,     ///< UINT_32 stored as INT32, widened to Int64.
  };

  DataType type;
  Conversion conversion = Conversion::kNone;
  TimeUnit unit = TimeUnit::kMicros;
};

/// @brief The DataType of a leaf column and how to read it.
/// @return The leaf type, or `TypeError` for FIXED_LEN_BYTE_ARRAY, UINT_64 and DECIMAL
/// columns of more than 18 digits.
[[nodiscard]] Result<LeafType> ResolveLeafType(const SchemaElement& elem);

/// @brief The schema of a file's leaf columns, and how to read each (same order).
struct FileSchema {
  Schema schema;
  std::vector<LeafType> leaves;
};

/// @return The file schema, or the `ResolveLeafType` error of the first unsupported column.
[[nodiscard]] Result<FileSchema> BuildSchema(const FileMetadata& metadata);

}  // namespace quest::io::parquet
