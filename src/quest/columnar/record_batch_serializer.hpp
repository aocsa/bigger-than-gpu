// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstring>
#include <fstream>
#include <memory>
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
//     - Type index (uint8_t) - index into DataType variant
//     - Type data (variable, e.g., byte_width for FixedLenByteArrayType)
//     - Repetition (uint8_t)
// - Column data section:
//   - For each column:
//     - Null bitmap size (uint32_t)
//     - Null bitmap (variable length)
//     - Data (format depends on type)

class RecordBatchSerializer {
 public:
  [[nodiscard]] static Result<std::vector<uint8_t>> Serialize(const RecordBatch& batch) {
    std::vector<uint8_t> buffer;

    const char magic[4] = {'Q', 'R', 'B', 'T'};
    buffer.insert(buffer.end(), magic, magic + 4);

    uint32_t version = 1;
    WriteValue(buffer, version);

    uint64_t num_rows = batch.num_rows();
    uint32_t num_columns = static_cast<uint32_t>(batch.num_columns());
    WriteValue(buffer, num_rows);
    WriteValue(buffer, num_columns);

    const Schema& schema = batch.schema();
    for (size_t i = 0; i < num_columns; ++i) {
      const Field& field = schema.field(i);

      uint32_t name_len = static_cast<uint32_t>(field.name.size());
      WriteValue(buffer, name_len);
      buffer.insert(buffer.end(), field.name.begin(), field.name.end());

      WriteDataType(buffer, field.data_type);

      uint8_t rep = static_cast<uint8_t>(field.repetition);
      WriteValue(buffer, rep);
    }

    for (size_t i = 0; i < num_columns; ++i) {
      const ColumnVector& column = batch.column(i);
      if (auto status = WriteColumn(buffer, column, num_rows); !status) {
        return std::unexpected(status.error());
      }
    }

    return buffer;
  }

  [[nodiscard]] static Result<RecordBatch> Deserialize(const std::vector<uint8_t>& buffer) {
    size_t offset = 0;

    if (buffer.size() < 4 ||
        buffer[0] != 'Q' || buffer[1] != 'R' ||
        buffer[2] != 'B' || buffer[3] != 'T') {
      return Error::Invalid("Invalid magic number");
    }
    offset += 4;

    auto version = ReadValue<uint32_t>(buffer, offset);
    if (!version) return std::unexpected(version.error());
    if (*version != 1) {
      return Error::Invalid("Unsupported version");
    }

    auto num_rows = ReadValue<uint64_t>(buffer, offset);
    if (!num_rows) return std::unexpected(num_rows.error());
    auto num_columns = ReadValue<uint32_t>(buffer, offset);
    if (!num_columns) return std::unexpected(num_columns.error());

    std::vector<Field> fields;
    fields.reserve(*num_columns);

    for (uint32_t i = 0; i < *num_columns; ++i) {
      auto name_len = ReadValue<uint32_t>(buffer, offset);
      if (!name_len) return std::unexpected(name_len.error());
      std::string name(buffer.begin() + offset, buffer.begin() + offset + *name_len);
      offset += *name_len;

      auto data_type = ReadDataType(buffer, offset);
      if (!data_type) return std::unexpected(data_type.error());

      auto rep = ReadValue<uint8_t>(buffer, offset);
      if (!rep) return std::unexpected(rep.error());
      Repetition repetition = static_cast<Repetition>(*rep);

      fields.emplace_back(std::move(name), std::move(*data_type), repetition);
    }

    Schema schema(std::move(fields));

    std::vector<std::shared_ptr<ColumnVector>> columns;
    columns.reserve(*num_columns);

    for (uint32_t i = 0; i < *num_columns; ++i) {
      auto column = ReadColumn(buffer, offset, schema.field(i).data_type, *num_rows);
      if (!column) return std::unexpected(column.error());
      columns.push_back(std::move(*column));
    }

    return RecordBatch(std::move(schema), std::move(columns));
  }

  [[nodiscard]] static Status SerializeToFile(const RecordBatch& batch,
                                              const std::string& filename) {
    auto data = Serialize(batch);
    if (!data) return std::unexpected(data.error());
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
      return Error::IOError("Failed to open file for writing: " + filename);
    }
    file.write(reinterpret_cast<const char*>(data->data()), data->size());
    return {};
  }

  [[nodiscard]] static Result<RecordBatch> DeserializeFromFile(const std::string& filename) {
    std::ifstream file(filename, std::ios::binary | std::ios::ate);
    if (!file) {
      return Error::IOError("Failed to open file for reading: " + filename);
    }

    std::streamsize size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(size);
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
      return Error::IOError("Failed to read file: " + filename);
    }

    return Deserialize(buffer);
  }

 private:
  template <typename T>
  static void WriteValue(std::vector<uint8_t>& buffer, const T& value) {
    const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
    buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
  }

  template <typename T>
  static Result<T> ReadValue(const std::vector<uint8_t>& buffer, size_t& offset) {
    if (offset + sizeof(T) > buffer.size()) {
      return Error::Invalid("Buffer overflow during deserialization");
    }
    T value;
    std::memcpy(&value, buffer.data() + offset, sizeof(T));
    offset += sizeof(T);
    return value;
  }

  static void WriteDataType(std::vector<uint8_t>& buffer, const DataType& type) {
    uint8_t type_index = static_cast<uint8_t>(type.index());
    WriteValue(buffer, type_index);

    // For FixedLenByteArrayType, write byte_width
    if (std::holds_alternative<FixedLenByteArrayType>(type)) {
      const auto& fixed_type = std::get<FixedLenByteArrayType>(type);
      WriteValue(buffer, fixed_type.byte_width);
    }
  }

  static Result<DataType> ReadDataType(const std::vector<uint8_t>& buffer, size_t& offset) {
    auto type_index = ReadValue<uint8_t>(buffer, offset);
    if (!type_index) return std::unexpected(type_index.error());

    switch (*type_index) {
      case 0: return BoolType{};
      case 1: return Int8Type{};
      case 2: return Int16Type{};
      case 3: return Int32Type{};
      case 4: return Int64Type{};
      case 5: return FloatType{};
      case 6: return DoubleType{};
      case 7: return StringType{};
      case 8: return BinaryType{};
      case 9: return Int96Type{};
      case 10: {
        auto byte_width = ReadValue<int32_t>(buffer, offset);
        if (!byte_width) return std::unexpected(byte_width.error());
        return FixedLenByteArrayType{*byte_width};
      }
      default:
        return Error::Invalid("Unknown type index");
    }
  }

  static Status WriteColumn(std::vector<uint8_t>& buffer, const ColumnVector& column, size_t num_rows) {
    size_t bitmap_bytes = (num_rows + 7) / 8;
    uint32_t bitmap_size = static_cast<uint32_t>(bitmap_bytes);
    WriteValue(buffer, bitmap_size);

    std::vector<uint8_t> null_bitmap(bitmap_bytes, 0);
    for (size_t i = 0; i < num_rows; ++i) {
      if (!column.IsNull(i)) {
        size_t byte_idx = i / 8;
        size_t bit_idx = i % 8;
        null_bitmap[byte_idx] |= (1 << bit_idx);
      }
    }
    buffer.insert(buffer.end(), null_bitmap.begin(), null_bitmap.end());

    return WriteColumnValues(buffer, column, num_rows);
  }

  static Status WriteColumnValues(std::vector<uint8_t>& buffer, const ColumnVector& column, size_t num_rows) {
    const DataType& type = column.GetType();

    return std::visit([&](const auto& t) -> Status {
      using T = std::decay_t<decltype(t)>;

      if constexpr (std::is_same_v<T, BoolType>) {
        for (size_t i = 0; i < num_rows; ++i) {
          bool val = column.IsNull(i) ? false : std::get<bool>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, Int8Type>) {
        for (size_t i = 0; i < num_rows; ++i) {
          int8_t val = column.IsNull(i) ? 0 : std::get<int8_t>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, Int16Type>) {
        for (size_t i = 0; i < num_rows; ++i) {
          int16_t val = column.IsNull(i) ? 0 : std::get<int16_t>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, Int32Type>) {
        for (size_t i = 0; i < num_rows; ++i) {
          int32_t val = column.IsNull(i) ? 0 : std::get<int32_t>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, Int64Type>) {
        for (size_t i = 0; i < num_rows; ++i) {
          int64_t val = column.IsNull(i) ? 0 : std::get<int64_t>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, FloatType>) {
        for (size_t i = 0; i < num_rows; ++i) {
          float val = column.IsNull(i) ? 0.0f : std::get<float>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, DoubleType>) {
        for (size_t i = 0; i < num_rows; ++i) {
          double val = column.IsNull(i) ? 0.0 : std::get<double>(column.GetValue(i));
          WriteValue(buffer, val);
        }
      } else if constexpr (std::is_same_v<T, StringType> || std::is_same_v<T, BinaryType>) {
        for (size_t i = 0; i < num_rows; ++i) {
          std::string val;
          if (!column.IsNull(i)) {
            val = std::get<std::string>(column.GetValue(i));
          }
          uint32_t len = static_cast<uint32_t>(val.size());
          WriteValue(buffer, len);
          buffer.insert(buffer.end(), val.begin(), val.end());
        }
      } else {
        return Error::TypeError("Unsupported type for serialization");
      }
      return {};
    }, type);
  }

  static Result<std::shared_ptr<ColumnVector>> ReadColumn(const std::vector<uint8_t>& buffer,
                                                           size_t& offset,
                                                           const DataType& type,
                                                           size_t num_rows) {
    auto bitmap_size = ReadValue<uint32_t>(buffer, offset);
    if (!bitmap_size) return std::unexpected(bitmap_size.error());
    std::vector<uint8_t> null_bitmap(buffer.begin() + offset,
                                     buffer.begin() + offset + *bitmap_size);
    offset += *bitmap_size;

    std::shared_ptr<ColumnVector> column = CreateColumnVector(type, num_rows);
    if (auto status = ReadColumnValues(buffer, offset, *column, type, num_rows, null_bitmap);
        !status) {
      return std::unexpected(status.error());
    }

    return column;
  }

  static Status ReadColumnValues(const std::vector<uint8_t>& buffer,
                                size_t& offset,
                                ColumnVector& column,
                                const DataType& type,
                                size_t num_rows,
                                const std::vector<uint8_t>& null_bitmap) {
    return std::visit([&](const auto& t) -> Status {
      using T = std::decay_t<decltype(t)>;

      auto is_null = [&](size_t i) {
        size_t byte_idx = i / 8;
        size_t bit_idx = i % 8;
        return (null_bitmap[byte_idx] & (1 << bit_idx)) == 0;
      };

      if constexpr (std::is_same_v<T, BoolType>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<bool>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<bool>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, Int8Type>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<int8_t>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<int8_t>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, Int16Type>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<int16_t>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<int16_t>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, Int32Type>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<int32_t>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<int32_t>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, Int64Type>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<int64_t>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<int64_t>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, FloatType>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<float>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<float>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, DoubleType>) {
        auto* typed_col = dynamic_cast<TypedColumnVector<double>*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto val = ReadValue<double>(buffer, offset);
          if (!val) return std::unexpected(val.error());
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(*val);
          }
        }
      } else if constexpr (std::is_same_v<T, StringType> || std::is_same_v<T, BinaryType>) {
        auto* typed_col = dynamic_cast<StringColumnVector*>(&column);
        for (size_t i = 0; i < num_rows; ++i) {
          auto len = ReadValue<uint32_t>(buffer, offset);
          if (!len) return std::unexpected(len.error());
          std::string val(buffer.begin() + offset, buffer.begin() + offset + *len);
          offset += *len;
          if (is_null(i)) {
            typed_col->AppendNull();
          } else {
            typed_col->Append(std::move(val));
          }
        }
      } else {
        return Error::TypeError("Unsupported type for deserialization");
      }
      return {};
    }, type);
  }
};

}  // namespace quest::columnar
