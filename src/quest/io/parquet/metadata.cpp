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

// TimestampType { 1: bool isAdjustedToUTC; 2: TimeUnit unit }, where TimeUnit is a union of
// empty structs: 1 MILLIS, 2 MICROS, 3 NANOS. QUEST ignores isAdjustedToUTC.
Status ReadTimestampType(ThriftDecoder& d, SchemaElement& elem) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    if (id != 2 || type != ThriftType::kStruct) {
      return d.SkipField(type);
    }
    return ReadStruct(d, [&](int16_t unit_id, ThriftType unit_type) -> Status {
      if (unit_id >= 1 && unit_id <= 3) {
        elem.time_unit = static_cast<TimeUnit>(unit_id - 1);
      }
      return d.SkipField(unit_type);
    });
  });
}

// IntType { 1: i8 bitWidth; 2: bool isSigned }; compact thrift puts a bool in its header.
Status ReadIntType(ThriftDecoder& d, SchemaElement& elem) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    if (id == 1 && type == ThriftType::kByte) {
      return d.ReadByte().transform(
          [&](uint8_t width) { elem.bit_width = static_cast<int8_t>(width); });
    }
    if (id == 2 && (type == ThriftType::kBoolTrue || type == ThriftType::kBoolFalse)) {
      elem.is_signed = type == ThriftType::kBoolTrue;
      return {};
    }
    return d.SkipField(type);
  });
}

// SchemaElement.logicalType is a union: the id of its one set field names the type.
Status ReadLogicalType(ThriftDecoder& d, SchemaElement& elem) {
  std::optional<LogicalType> logical;
  auto read = ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    logical = FromLogicalTypeField(id);
    if (type == ThriftType::kStruct && logical == LogicalType::kTimestamp) {
      return ReadTimestampType(d, elem);
    }
    if (type == ThriftType::kStruct && logical == LogicalType::kInteger) {
      return ReadIntType(d, elem);
    }
    return d.SkipField(type);
  });
  if (read && logical) {
    elem.logical_type = logical;
  }
  return read;
}

}  // namespace

// The struct readers below are static rather than in the anonymous namespace: the
// Read(std::vector<T>&) and Read(std::optional<T>&) templates find them by ADL.

static Status Read(ThriftDecoder& d, KeyValue& kv) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, kv.key);
      case 2: return Read(d, kv.value);
      default: return d.SkipField(type);
    }
  });
}

static Status Read(ThriftDecoder& d, ColumnStatistics& stats) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, stats.max_value);  // deprecated max, but still written
      case 2: return Read(d, stats.min_value);  // deprecated min, but still written
      case 3: return Read(d, stats.null_count);
      case 4: return Read(d, stats.distinct_count);
      case 5: return stats.max_value ? d.SkipField(type) : Read(d, stats.max_value);
      case 6: return stats.min_value ? d.SkipField(type) : Read(d, stats.min_value);
      default: return d.SkipField(type);
    }
  });
}

// ColumnChunk.meta_data, read into the chunk itself.
static Status ReadColumnMetaData(ThriftDecoder& d, ColumnChunkMetadata& cc) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, cc.type);
      case 2: return Read(d, cc.encodings);
      case 3: return Read(d, cc.path_in_schema);
      case 4: return Read(d, cc.codec);
      case 5: return Read(d, cc.num_values);
      case 6: return Read(d, cc.total_uncompressed_size);
      case 7: return Read(d, cc.total_compressed_size);
      case 9: return Read(d, cc.data_page_offset);
      case 10: return Read(d, cc.index_page_offset);
      case 11: return Read(d, cc.dictionary_page_offset);
      case 12:  // replaces earlier statistics, as fields 5 and 6 look at what 1 and 2 set
        cc.statistics = {};
        return Read(d, cc.statistics);
      default: return d.SkipField(type);
    }
  });
}

static Status Read(ThriftDecoder& d, ColumnChunkMetadata& cc) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, cc.file_path);
      case 2: return Read(d, cc.file_offset);
      case 3: return ReadColumnMetaData(d, cc);
      default: return d.SkipField(type);
    }
  });
}

static Status Read(ThriftDecoder& d, RowGroupMetadata& rg) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, rg.columns);
      case 2: return Read(d, rg.total_byte_size);
      case 3: return Read(d, rg.num_rows);
      case 4: return Read(d, rg.file_offset);
      case 5: return Read(d, rg.total_compressed_size);
      case 6: return Read(d, rg.ordinal);
      default: return d.SkipField(type);
    }
  });
}

static Status Read(ThriftDecoder& d, SchemaElement& elem) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, elem.type);
      case 2: return Read(d, elem.type_length);
      case 3: return Read(d, elem.repetition);
      case 4: return Read(d, elem.name);
      case 5: return Read(d, elem.num_children);
      case 6:  // converted_type (legacy; logicalType takes precedence)
        return d.ReadI32().transform([&](int32_t converted) {
          if (!elem.logical_type) {
            elem.logical_type = FromConvertedType(converted);
          }
          ApplyConvertedTypeDetails(converted, elem);
        });
      case 7: return Read(d, elem.scale);
      case 8: return Read(d, elem.precision);
      case 9: return Read(d, elem.field_id);
      case 10: return ReadLogicalType(d, elem);
      default: return d.SkipField(type);
    }
  });
}

static Status Read(ThriftDecoder& d, FileMetadata& metadata) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, metadata.version);
      case 2: return Read(d, metadata.schema_elements);
      case 3: return Read(d, metadata.num_rows);
      case 4: return Read(d, metadata.row_groups);
      case 5: return Read(d, metadata.key_value_metadata);
      case 6: return Read(d, metadata.created_by);
      default: return d.SkipField(type);
    }
  });
}

Result<FileMetadata> MetadataParser::Parse(std::span<const uint8_t> data) {
  ThriftDecoder decoder(data);
  FileMetadata metadata;
  return Read(decoder, metadata).transform([&] { return std::move(metadata); });
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
