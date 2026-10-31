// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

/// @file metadata.hpp
/// @brief Parquet file metadata structures and Thrift footer parser.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "quest/datasource/schema.hpp"
#include "quest/datasource/types.hpp"
#include "quest/datasource/parquet/thrift_decoder.hpp"
#include "quest/result.hpp"

namespace quest::datasource::parquet {

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
      std::span<const uint8_t> data) {
    ThriftDecoder decoder(data);
    return ParseFileMetadata(decoder);
  }

 private:
  // ===========================================================================
  // Top-Level Parsers
  // ===========================================================================

  [[nodiscard]] static Result<FileMetadata> ParseFileMetadata(
      ThriftDecoder& decoder) {
    FileMetadata metadata;
    int16_t last_field_id = 0;

    while (true) {
      // Read field header.
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;  // STOP field.
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {  // version
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          metadata.version = result.value();
          break;
        }
        case 2: {  // schema
          auto result = ParseSchemaElements(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          metadata.schema_elements = std::move(result.value());
          break;
        }
        case 3: {  // num_rows
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          metadata.num_rows = result.value();
          break;
        }
        case 4: {  // row_groups
          auto result = ParseRowGroups(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          metadata.row_groups = std::move(result.value());
          break;
        }
        case 5: {  // key_value_metadata
          auto result = ParseKeyValueMetadata(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          metadata.key_value_metadata = std::move(result.value());
          break;
        }
        case 6: {  // created_by
          auto result = decoder.ReadString();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          metadata.created_by = std::move(result.value());
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
      }
    }
    return metadata;
  }

  // ===========================================================================
  // Schema Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<SchemaElement>> ParseSchemaElements(
      ThriftDecoder& decoder) {
    auto list_header_result = decoder.ReadListHeader();
    if (!list_header_result.has_value()) {
      return std::unexpected(list_header_result.error());
    }
    auto list_header = list_header_result.value();

    std::vector<SchemaElement> elements;
    elements.reserve(list_header.size);

    for (uint32_t i = 0; i < list_header.size; ++i) {
      auto elem_result = ParseSchemaElement(decoder);
      if (!elem_result.has_value()) {
        return std::unexpected(elem_result.error());
      }
      elements.push_back(std::move(elem_result.value()));
    }
    return elements;
  }

  [[nodiscard]] static Result<SchemaElement> ParseSchemaElement(
      ThriftDecoder& decoder) {
    SchemaElement elem;
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {  // type (physical type)
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          elem.type = static_cast<PhysicalType>(result.value());
          break;
        }
        case 2: {  // type_length
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          elem.type_length = result.value();
          break;
        }
        case 3: {  // repetition_type
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          elem.repetition = static_cast<Repetition>(result.value());
          break;
        }
        case 4: {  // name
          auto result = decoder.ReadString();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          elem.name = std::move(result.value());
          break;
        }
        case 5: {  // num_children
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          elem.num_children = result.value();
          break;
        }
        case 7: {  // field_id
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          elem.field_id = result.value();
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
      }
    }
    return elem;
  }

  // ===========================================================================
  // Row Group Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<RowGroupMetadata>> ParseRowGroups(
      ThriftDecoder& decoder) {
    auto list_header_result = decoder.ReadListHeader();
    if (!list_header_result.has_value()) {
      return std::unexpected(list_header_result.error());
    }
    auto list_header = list_header_result.value();

    std::vector<RowGroupMetadata> row_groups;
    row_groups.reserve(list_header.size);

    for (uint32_t i = 0; i < list_header.size; ++i) {
      auto rg_result = ParseRowGroup(decoder);
      if (!rg_result.has_value()) {
        return std::unexpected(rg_result.error());
      }
      row_groups.push_back(std::move(rg_result.value()));
    }
    return row_groups;
  }

  [[nodiscard]] static Result<RowGroupMetadata> ParseRowGroup(
      ThriftDecoder& decoder) {
    RowGroupMetadata rg;
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {  // columns
          auto result = ParseColumnChunks(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          rg.columns = std::move(result.value());
          break;
        }
        case 2: {  // total_byte_size
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          rg.total_byte_size = result.value();
          break;
        }
        case 3: {  // num_rows
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          rg.num_rows = result.value();
          break;
        }
        case 4: {  // file_offset
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          rg.file_offset = result.value();
          break;
        }
        case 5: {  // total_compressed_size
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          rg.total_compressed_size = result.value();
          break;
        }
        case 6: {  // ordinal
          auto result = decoder.ReadI16();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          rg.ordinal = result.value();
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
      }
    }
    return rg;
  }

  // ===========================================================================
  // Column Chunk Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<ColumnChunkMetadata>> ParseColumnChunks(
      ThriftDecoder& decoder) {
    auto list_header_result = decoder.ReadListHeader();
    if (!list_header_result.has_value()) {
      return std::unexpected(list_header_result.error());
    }
    auto list_header = list_header_result.value();

    std::vector<ColumnChunkMetadata> columns;
    columns.reserve(list_header.size);

    for (uint32_t i = 0; i < list_header.size; ++i) {
      auto cc_result = ParseColumnChunk(decoder);
      if (!cc_result.has_value()) {
        return std::unexpected(cc_result.error());
      }
      columns.push_back(std::move(cc_result.value()));
    }
    return columns;
  }

  [[nodiscard]] static Result<ColumnChunkMetadata> ParseColumnChunk(
      ThriftDecoder& decoder) {
    ColumnChunkMetadata cc;
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {  // file_path
          auto result = decoder.ReadString();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.file_path = std::move(result.value());
          break;
        }
        case 2: {  // file_offset
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.file_offset = result.value();
          break;
        }
        case 3: {  // meta_data
          auto status = ParseColumnMetadata(decoder, cc);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
      }
    }
    return cc;
  }

  [[nodiscard]] static Status ParseColumnMetadata(ThriftDecoder& decoder,
                                                   ColumnChunkMetadata& cc) {
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {  // type
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.type = static_cast<PhysicalType>(result.value());
          break;
        }
        case 2: {  // encodings
          auto result = ParseEncodings(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.encodings = std::move(result.value());
          break;
        }
        case 3: {  // path_in_schema
          auto result = ParseStringList(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.path_in_schema = std::move(result.value());
          break;
        }
        case 4: {  // codec (compression)
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.codec = static_cast<CompressionType>(result.value());
          break;
        }
        case 5: {  // num_values
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.num_values = result.value();
          break;
        }
        case 6: {  // total_uncompressed_size
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.total_uncompressed_size = result.value();
          break;
        }
        case 7: {  // total_compressed_size
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.total_compressed_size = result.value();
          break;
        }
        case 9: {  // data_page_offset
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.data_page_offset = result.value();
          break;
        }
        case 10: {  // index_page_offset
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.index_page_offset = result.value();
          break;
        }
        case 11: {  // dictionary_page_offset
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.dictionary_page_offset = result.value();
          break;
        }
        case 12: {  // statistics
          auto result = ParseStatistics(decoder);
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          cc.statistics = std::move(result.value());
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return status;
          }
          break;
        }
      }
    }
    return Status::OK();
  }

  // ===========================================================================
  // Helper Parsers
  // ===========================================================================

  [[nodiscard]] static Result<std::vector<Encoding>> ParseEncodings(
      ThriftDecoder& decoder) {
    auto list_header_result = decoder.ReadListHeader();
    if (!list_header_result.has_value()) {
      return std::unexpected(list_header_result.error());
    }
    auto list_header = list_header_result.value();

    std::vector<Encoding> encodings;
    encodings.reserve(list_header.size);

    for (uint32_t i = 0; i < list_header.size; ++i) {
      auto result = decoder.ReadI32();
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      encodings.push_back(static_cast<Encoding>(result.value()));
    }
    return encodings;
  }

  [[nodiscard]] static Result<std::vector<std::string>> ParseStringList(
      ThriftDecoder& decoder) {
    auto list_header_result = decoder.ReadListHeader();
    if (!list_header_result.has_value()) {
      return std::unexpected(list_header_result.error());
    }
    auto list_header = list_header_result.value();

    std::vector<std::string> strings;
    strings.reserve(list_header.size);

    for (uint32_t i = 0; i < list_header.size; ++i) {
      auto result = decoder.ReadString();
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      strings.push_back(std::move(result.value()));
    }
    return strings;
  }

  [[nodiscard]] static Result<ColumnStatistics> ParseStatistics(
      ThriftDecoder& decoder) {
    ColumnStatistics stats;
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {  // max (deprecated, but still used)
          auto result = decoder.ReadBinary();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          stats.max_value = std::move(result.value());
          break;
        }
        case 2: {  // min (deprecated, but still used)
          auto result = decoder.ReadBinary();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          stats.min_value = std::move(result.value());
          break;
        }
        case 3: {  // null_count
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          stats.null_count = result.value();
          break;
        }
        case 4: {  // distinct_count
          auto result = decoder.ReadI64();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          stats.distinct_count = result.value();
          break;
        }
        case 5: {  // max_value (new field)
          if (!stats.max_value) {
            auto result = decoder.ReadBinary();
            if (!result.has_value()) {
              return std::unexpected(result.error());
            }
            stats.max_value = std::move(result.value());
          } else {
            auto status = decoder.SkipField(header->type);
            if (!status.ok()) {
              return std::unexpected(status.error());
            }
          }
          break;
        }
        case 6: {  // min_value (new field)
          if (!stats.min_value) {
            auto result = decoder.ReadBinary();
            if (!result.has_value()) {
              return std::unexpected(result.error());
            }
            stats.min_value = std::move(result.value());
          } else {
            auto status = decoder.SkipField(header->type);
            if (!status.ok()) {
              return std::unexpected(status.error());
            }
          }
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
      }
    }
    return stats;
  }

  [[nodiscard]] static Result<std::vector<KeyValue>> ParseKeyValueMetadata(
      ThriftDecoder& decoder) {
    auto list_header_result = decoder.ReadListHeader();
    if (!list_header_result.has_value()) {
      return std::unexpected(list_header_result.error());
    }
    auto list_header = list_header_result.value();

    std::vector<KeyValue> kvs;
    kvs.reserve(list_header.size);

    for (uint32_t i = 0; i < list_header.size; ++i) {
      auto kv_result = ParseKeyValue(decoder);
      if (!kv_result.has_value()) {
        return std::unexpected(kv_result.error());
      }
      kvs.push_back(std::move(kv_result.value()));
    }
    return kvs;
  }

  [[nodiscard]] static Result<KeyValue> ParseKeyValue(ThriftDecoder& decoder) {
    KeyValue kv;
    int16_t last_field_id = 0;

    while (true) {
      auto header_result = decoder.ReadFieldHeader(last_field_id);
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();
      if (!header) {
        break;
      }
      last_field_id = header->field_id;

      switch (header->field_id) {
        case 1: {
          auto result = decoder.ReadString();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          kv.key = std::move(result.value());
          break;
        }
        case 2: {
          auto result = decoder.ReadString();
          if (!result.has_value()) {
            return std::unexpected(result.error());
          }
          kv.value = std::move(result.value());
          break;
        }
        default: {
          auto status = decoder.SkipField(header->type);
          if (!status.ok()) {
            return std::unexpected(status.error());
          }
          break;
        }
      }
    }
    return kv;
  }
};

// =============================================================================
// Schema Building
// =============================================================================

inline Schema BuildSchema(const FileMetadata& metadata) {
  std::vector<Field> fields;

  // Skip the root schema element (index 0) and process leaf columns.
  size_t i = 1;
  while (i < metadata.schema_elements.size()) {
    const auto& elem = metadata.schema_elements[i];
    if (elem.IsLeaf()) {
      DataType data_type =
          PhysicalTypeToDataType(elem.type, elem.type_length.value_or(0));
      fields.emplace_back(elem.name, data_type, elem.repetition);
      if (elem.field_id) {
        fields.back().field_id = elem.field_id;
      }
      if (elem.type_length) {
        fields.back().type_length = elem.type_length;
      }
    }
    ++i;
  }

  return Schema(std::move(fields));
}

}  // namespace quest::datasource::parquet
