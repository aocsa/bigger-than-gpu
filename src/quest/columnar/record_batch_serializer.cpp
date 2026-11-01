// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/columnar/record_batch_serializer.hpp"

#include <cstring>
#include <fstream>
#include <type_traits>
#include <utility>
#include <variant>

#include "quest/columnar/column_vector.hpp"

namespace quest::columnar {

namespace {

template <typename T>
void WriteValue(std::vector<uint8_t>& buffer, const T& value) {
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&value);
  buffer.insert(buffer.end(), ptr, ptr + sizeof(T));
}

template <typename T>
Result<T> ReadValue(const std::vector<uint8_t>& buffer, size_t& offset) {
  if (offset + sizeof(T) > buffer.size()) {
    return Error::Invalid("Buffer overflow during deserialization");
  }
  T value;
  std::memcpy(&value, buffer.data() + offset, sizeof(T));
  offset += sizeof(T);
  return value;
}

}  // namespace

Result<std::vector<uint8_t>> RecordBatchSerializer::Serialize(const RecordBatch& batch) {
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

Result<RecordBatch> RecordBatchSerializer::Deserialize(const std::vector<uint8_t>& buffer) {
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

  std::vector<ColumnVectorPtr> columns;
  columns.reserve(*num_columns);

  for (uint32_t i = 0; i < *num_columns; ++i) {
    auto column = ReadColumn(buffer, offset, schema.field(i).data_type, *num_rows);
    if (!column) return std::unexpected(column.error());
    columns.push_back(std::move(*column));
  }

  return RecordBatch(std::move(schema), std::move(columns));
}

Status RecordBatchSerializer::SerializeToFile(const RecordBatch& batch,
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

Result<RecordBatch> RecordBatchSerializer::DeserializeFromFile(const std::string& filename) {
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

void RecordBatchSerializer::WriteDataType(std::vector<uint8_t>& buffer, DataType type) {
  WriteValue(buffer, std::to_underlying(type));
}

Result<DataType> RecordBatchSerializer::ReadDataType(const std::vector<uint8_t>& buffer,
                                                     size_t& offset) {
  auto tag = ReadValue<std::underlying_type_t<DataType>>(buffer, offset);
  if (!tag) return std::unexpected(tag.error());
  if (*tag > std::to_underlying(DataType::kTimestamp)) {
    return Error::Invalid("Unknown type index");
  }
  return static_cast<DataType>(*tag);
}

Status RecordBatchSerializer::WriteColumn(std::vector<uint8_t>& buffer, const ColumnVector& column,
                                          size_t num_rows) {
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

Status RecordBatchSerializer::WriteColumnValues(std::vector<uint8_t>& buffer,
                                                const ColumnVector& column, size_t num_rows) {
  return VisitType(column.GetType(), [&]<typename Tag>(Tag) -> Status {
    using T = ValueOf<typename Tag::CType>;
    for (size_t i = 0; i < num_rows; ++i) {
      const T val = column.IsNull(i) ? T{} : std::get<T>(column.GetValue(i));
      if constexpr (std::same_as<T, std::string>) {
        WriteValue(buffer, static_cast<uint32_t>(val.size()));
        buffer.insert(buffer.end(), val.begin(), val.end());
      } else {
        WriteValue(buffer, val);
      }
    }
    return {};
  });
}

Result<ColumnVectorPtr> RecordBatchSerializer::ReadColumn(const std::vector<uint8_t>& buffer,
                                                          size_t& offset, DataType type,
                                                          size_t num_rows) {
  auto bitmap_size = ReadValue<uint32_t>(buffer, offset);
  if (!bitmap_size) return std::unexpected(bitmap_size.error());
  std::vector<uint8_t> null_bitmap(buffer.begin() + offset,
                                   buffer.begin() + offset + *bitmap_size);
  offset += *bitmap_size;

  return VisitType(type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = ValueOf<typename Tag::CType>;
    ColumnBuilder<typename Tag::CType> builder(type, num_rows);
    for (size_t i = 0; i < num_rows; ++i) {
      T val{};
      if constexpr (std::same_as<T, std::string>) {
        auto len = ReadValue<uint32_t>(buffer, offset);
        if (!len) return std::unexpected(len.error());
        val.assign(buffer.begin() + offset, buffer.begin() + offset + *len);
        offset += *len;
      } else {
        auto read = ReadValue<T>(buffer, offset);
        if (!read) return std::unexpected(read.error());
        val = *read;
      }
      if ((null_bitmap[i / 8] & (1 << (i % 8))) == 0) {
        builder.AppendNull();
      } else {
        builder.Append(val);
      }
    }
    return std::move(builder).Finish();
  });
}

}  // namespace quest::columnar
