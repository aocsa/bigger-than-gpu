// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/io/parquet/metadata.hpp"

#include <utility>

#include "quest/io/parquet/thrift_decoder.hpp"

namespace quest::io::parquet {

namespace {

// Legacy SchemaElement.converted_type values (parquet.thrift ConvertedType).
std::optional<LogicalType> FromConvertedType(int32_t converted) {
  switch (converted) {
    case 0: return LogicalType::kString;  // UTF8
    case 1:                                // MAP
    case 2: return LogicalType::kMap;      // MAP_KEY_VALUE
    case 3: return LogicalType::kList;
    case 4: return LogicalType::kEnum;
    case 5: return LogicalType::kDecimal;
    case 6: return LogicalType::kDate;
    case 7:                                // TIME_MILLIS
    case 8: return LogicalType::kTime;     // TIME_MICROS
    case 9:                                // TIMESTAMP_MILLIS
    case 10: return LogicalType::kTimestamp;  // TIMESTAMP_MICROS
    case 19: return LogicalType::kJson;
    case 20: return LogicalType::kBson;
    default:
      if (converted >= 11 && converted <= 18) {  // UINT_8 .. INT_64
        return LogicalType::kInteger;
      }
      return std::nullopt;  // INTERVAL or unknown
  }
}

// SchemaElement.logicalType is a thrift union: the id of its one set field names the type.
std::optional<LogicalType> FromLogicalTypeField(int16_t field_id) {
  switch (field_id) {
    case 1: return LogicalType::kString;
    case 2: return LogicalType::kMap;
    case 3: return LogicalType::kList;
    case 4: return LogicalType::kEnum;
    case 5: return LogicalType::kDecimal;
    case 6: return LogicalType::kDate;
    case 7: return LogicalType::kTime;
    case 8: return LogicalType::kTimestamp;
    case 10: return LogicalType::kInteger;
    case 12: return LogicalType::kJson;
    case 13: return LogicalType::kBson;
    case 14: return LogicalType::kUuid;
    default: return std::nullopt;
  }
}

// The unit and integer details of a legacy converted_type; a logicalType read later wins.
void ApplyConvertedTypeDetails(int32_t converted, SchemaElement& elem) {
  if (converted == 9 || converted == 10) {  // TIMESTAMP_MILLIS, TIMESTAMP_MICROS
    elem.time_unit = converted == 9 ? TimeUnit::kMillis : TimeUnit::kMicros;
  } else if (converted >= 11 && converted <= 18) {  // UINT_8 .. UINT_64, INT_8 .. INT_64
    elem.is_signed = converted >= 15;
    elem.bit_width = 8 << ((converted - 11) % 4);
  }
}

// Reads a thrift struct's fields, calling read_field(field_id, type) for each; it returns
// true if it read the value and false to have the value skipped.
template <typename ReadField>
Status ReadStructFields(ThriftDecoder& decoder, ReadField&& read_field) {
  int16_t last_field_id = 0;
  while (true) {
    auto header = decoder.ReadFieldHeader(last_field_id);
    if (!header) {
      return std::unexpected(std::move(header).error());
    }
    if (!*header) {
      return {};
    }
    last_field_id = (*header)->field_id;
    auto read = read_field(last_field_id, (*header)->type);
    if (!read) {
      return std::unexpected(std::move(read).error());
    }
    if (!*read) {
      if (auto skipped = decoder.SkipField((*header)->type); !skipped) {
        return skipped;
      }
    }
  }
}

// TimestampType { 1: bool isAdjustedToUTC; 2: TimeUnit unit }, where TimeUnit is a union of
// empty structs: 1 MILLIS, 2 MICROS, 3 NANOS. QUEST ignores isAdjustedToUTC.
Status ReadTimestampType(ThriftDecoder& decoder, SchemaElement& elem) {
  return ReadStructFields(decoder, [&](int16_t id, ThriftType type) -> Result<bool> {
    if (id != 2 || type != ThriftType::kStruct) {
      return false;
    }
    auto unit = ReadStructFields(decoder, [&](int16_t unit_id, ThriftType) -> Result<bool> {
      if (unit_id >= 1 && unit_id <= 3) {
        elem.time_unit = static_cast<TimeUnit>(unit_id - 1);
      }
      return false;
    });
    if (!unit) {
      return std::unexpected(std::move(unit).error());
    }
    return true;
  });
}

// IntType { 1: i8 bitWidth; 2: bool isSigned }; compact thrift puts a bool in its header.
Status ReadIntType(ThriftDecoder& decoder, SchemaElement& elem) {
  return ReadStructFields(decoder, [&](int16_t id, ThriftType type) -> Result<bool> {
    if (id == 1 && type == ThriftType::kByte) {
      auto width = decoder.ReadByte();
      if (!width) {
        return std::unexpected(std::move(width).error());
      }
      elem.bit_width = static_cast<int8_t>(*width);
      return true;
    }
    if (id == 2 && (type == ThriftType::kBoolTrue || type == ThriftType::kBoolFalse)) {
      elem.is_signed = type == ThriftType::kBoolTrue;
      return true;
    }
    return false;
  });
}

// SchemaElement.logicalType is a union: the id of its one set field names the type.
Result<std::optional<LogicalType>> ReadLogicalTypeUnion(ThriftDecoder& decoder,
                                                        SchemaElement& elem) {
  std::optional<LogicalType> logical;
  auto status = ReadStructFields(decoder, [&](int16_t id, ThriftType type) -> Result<bool> {
    logical = FromLogicalTypeField(id);
    if (type != ThriftType::kStruct) {
      return false;
    }
    if (logical == LogicalType::kTimestamp) {
      auto read = ReadTimestampType(decoder, elem);
      return read ? Result<bool>{true} : std::unexpected(std::move(read).error());
    }
    if (logical == LogicalType::kInteger) {
      auto read = ReadIntType(decoder, elem);
      return read ? Result<bool>{true} : std::unexpected(std::move(read).error());
    }
    return false;
  });
  if (!status) {
    return std::unexpected(std::move(status).error());
  }
  return logical;
}

}  // namespace

Result<FileMetadata> MetadataParser::Parse(
    std::span<const uint8_t> data) {
  ThriftDecoder decoder(data);
  return ParseFileMetadata(decoder);
}

Result<FileMetadata> MetadataParser::ParseFileMetadata(
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
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return metadata;
}

Result<std::vector<SchemaElement>> MetadataParser::ParseSchemaElements(
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

Result<SchemaElement> MetadataParser::ParseSchemaElement(
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
      case 6: {  // converted_type (legacy; logicalType below takes precedence)
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        if (!elem.logical_type) {
          elem.logical_type = FromConvertedType(result.value());
        }
        ApplyConvertedTypeDetails(result.value(), elem);
        break;
      }
      case 7: {  // scale
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        elem.scale = result.value();
        break;
      }
      case 8: {  // precision
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        elem.precision = result.value();
        break;
      }
      case 9: {  // field_id
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        elem.field_id = result.value();
        break;
      }
      case 10: {  // logicalType
        auto result = ReadLogicalTypeUnion(decoder, elem);
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        if (*result) {
          elem.logical_type = *result;
        }
        break;
      }
      default: {
        auto status = decoder.SkipField(header->type);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return elem;
}

Result<std::vector<RowGroupMetadata>> MetadataParser::ParseRowGroups(
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

Result<RowGroupMetadata> MetadataParser::ParseRowGroup(
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
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return rg;
}

Result<std::vector<ColumnChunkMetadata>> MetadataParser::ParseColumnChunks(
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

Result<ColumnChunkMetadata> MetadataParser::ParseColumnChunk(
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
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      default: {
        auto status = decoder.SkipField(header->type);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return cc;
}

Status MetadataParser::ParseColumnMetadata(ThriftDecoder& decoder,
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
        if (!status.has_value()) {
          return status;
        }
        break;
      }
    }
  }
  return {};
}

Result<std::vector<Encoding>> MetadataParser::ParseEncodings(
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

Result<std::vector<std::string>> MetadataParser::ParseStringList(
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

Result<ColumnStatistics> MetadataParser::ParseStatistics(
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
          if (!status.has_value()) {
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
          if (!status.has_value()) {
            return std::unexpected(status.error());
          }
        }
        break;
      }
      default: {
        auto status = decoder.SkipField(header->type);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return stats;
}

Result<std::vector<KeyValue>> MetadataParser::ParseKeyValueMetadata(
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

Result<KeyValue> MetadataParser::ParseKeyValue(ThriftDecoder& decoder) {
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
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return kv;
}

Result<LeafType> ResolveLeafType(const SchemaElement& elem) {
  using Conversion = LeafType::Conversion;
  const auto logical = elem.logical_type;
  switch (elem.type) {
    case PhysicalType::kBoolean:
      return LeafType{DataType::kBool};
    case PhysicalType::kFloat:
      return LeafType{DataType::kFloat};
    case PhysicalType::kDouble:
      return LeafType{DataType::kDouble};
    case PhysicalType::kByteArray:
      return LeafType{DataType::kString};  // text or not, the bytes as they are
    case PhysicalType::kInt96:
      return LeafType{DataType::kTimestamp, Conversion::kInt96};
    case PhysicalType::kFixedLenByteArray:
      return Error::TypeError("Parquet column '" + elem.name +
                              "': FIXED_LEN_BYTE_ARRAY is not supported");
    case PhysicalType::kInt32:
    case PhysicalType::kInt64:
      break;
  }
  const bool is_int64 = elem.type == PhysicalType::kInt64;
  if (logical == LogicalType::kDecimal) {
    const int precision = elem.precision.value_or(DataType::kMaxDecimalPrecision);
    const int scale = elem.scale.value_or(0);
    if (precision < 1 || precision > DataType::kMaxDecimalPrecision || scale < 0 ||
        scale > precision) {
      return Error::TypeError(std::format("Parquet column '{}': DECIMAL({},{}) is not supported",
                                          elem.name, precision, scale));
    }
    return LeafType{DataType::Decimal(static_cast<std::uint8_t>(precision),
                                      static_cast<std::uint8_t>(scale)),
                    Conversion::kDecimal};
  }
  if (logical == LogicalType::kDate && !is_int64) {
    return LeafType{DataType::kDate32};
  }
  if (logical == LogicalType::kTimestamp && is_int64) {
    return LeafType{DataType::kTimestamp, Conversion::kTimestamp,
                    elem.time_unit.value_or(TimeUnit::kMicros)};
  }
  if (logical == LogicalType::kInteger && elem.is_signed == false) {
    if (elem.bit_width == 64) {
      return Error::TypeError("Parquet column '" + elem.name + "': UINT_64 is not supported");
    }
    if (elem.bit_width == 32) {
      return LeafType{DataType::kInt64, Conversion::kUInt32};
    }
  }
  return LeafType{is_int64 ? DataType::kInt64 : DataType::kInt32};
}

Result<FileSchema> BuildSchema(const FileMetadata& metadata) {
  FileSchema file_schema;
  std::vector<Field> fields;

  // Skip the root schema element (index 0) and process leaf columns.
  for (size_t i = 1; i < metadata.schema_elements.size(); ++i) {
    const auto& elem = metadata.schema_elements[i];
    if (!elem.IsLeaf()) {
      continue;
    }
    auto leaf = ResolveLeafType(elem);
    if (!leaf) {
      return std::unexpected(std::move(leaf).error());
    }
    fields.emplace_back(elem.name, leaf->type, elem.repetition);
    if (elem.field_id) {
      fields.back().field_id = elem.field_id;
    }
    if (elem.type_length) {
      fields.back().type_length = elem.type_length;
    }
    file_schema.leaves.push_back(*leaf);
  }
  file_schema.schema = Schema(std::move(fields));
  return file_schema;
}

}  // namespace quest::io::parquet
