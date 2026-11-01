// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/columnar/record_batch_serializer.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
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

  uint32_t version = 2;
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
  if (*version != 2) {
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
    if (*name_len > buffer.size() - offset) {
      return Error::Invalid("Buffer overflow during deserialization");
    }
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
  WriteValue(buffer, std::to_underlying(type.id));
  WriteValue(buffer, type.precision);
  WriteValue(buffer, type.scale);
}

Result<DataType> RecordBatchSerializer::ReadDataType(const std::vector<uint8_t>& buffer,
                                                     size_t& offset) {
  auto tag = ReadValue<std::underlying_type_t<TypeId>>(buffer, offset);
  if (!tag) return std::unexpected(tag.error());
  if (*tag > std::to_underlying(TypeId::kDecimal)) {
    return Error::Invalid("Unknown type index");
  }
  auto precision = ReadValue<uint8_t>(buffer, offset);
  if (!precision) return std::unexpected(precision.error());
  auto scale = ReadValue<uint8_t>(buffer, offset);
  if (!scale) return std::unexpected(scale.error());
  return DataType{static_cast<TypeId>(*tag), *precision, *scale};
}

Status RecordBatchSerializer::WriteColumn(std::vector<uint8_t>& buffer, const ColumnVector& column,
                                          size_t num_rows) {
  if (column.is_constant() || column.size() != num_rows) {
    return Error::Invalid("Spill writes only plain columns of the batch's row count");
  }
  WriteValue(buffer, static_cast<uint64_t>(column.null_count_));
  for (const Buffer* part : {&column.validity_, &column.values_, &column.offsets_}) {
    WriteValue(buffer, static_cast<uint64_t>(part->bytes.size()));
    const auto* bytes = reinterpret_cast<const uint8_t*>(part->bytes.data());
    buffer.insert(buffer.end(), bytes, bytes + part->bytes.size());
  }
  return {};
}

namespace {

/// @brief The next length-prefixed buffer, copied into a container of the column's element
/// type so typed reads see real objects.
/// @param expected_bytes The size the column needs; nullopt accepts any size (String chars).
/// @param may_be_empty An empty buffer is also valid (validity of a column with no nulls).
template <typename Container>
[[nodiscard]] Result<Container> ReadPart(const std::vector<uint8_t>& buffer, size_t& offset,
                                         std::optional<size_t> expected_bytes,
                                         bool may_be_empty = false) {
  auto length = ReadValue<uint64_t>(buffer, offset);
  if (!length) return std::unexpected(length.error());
  const bool size_ok =
      !expected_bytes || *length == *expected_bytes || (may_be_empty && *length == 0);
  if (*length > buffer.size() - offset || !size_ok) {
    return Error::Invalid("Spilled column buffer has the wrong size");
  }
  using Element = typename Container::value_type;
  Container part(*length / sizeof(Element), Element{});
  std::memcpy(part.data(), buffer.data() + offset, *length);
  offset += *length;
  return part;
}

}  // namespace

Result<ColumnVectorPtr> RecordBatchSerializer::ReadColumn(const std::vector<uint8_t>& buffer,
                                                          size_t& offset, DataType type,
                                                          size_t num_rows) {
  auto null_count = ReadValue<uint64_t>(buffer, offset);
  if (!null_count) return std::unexpected(null_count.error());
  auto validity =
      ReadPart<std::vector<uint8_t>>(buffer, offset, (num_rows + 7) / 8, /*may_be_empty=*/true);
  if (!validity) return std::unexpected(validity.error());
  if ((*null_count == 0) != validity->empty()) {
    return Error::Invalid("Spilled column has a null count but no bitmap, or the reverse");
  }

  return VisitType(type, [&]<typename Tag>(Tag) -> Result<ColumnVectorPtr> {
    using T = typename Tag::CType;
    Buffer values;
    Buffer offsets;
    if constexpr (std::same_as<T, std::string_view>) {
      auto chars = ReadPart<std::string>(buffer, offset, std::nullopt);
      if (!chars) return std::unexpected(chars.error());
      auto ends = ReadPart<std::vector<int64_t>>(buffer, offset, (num_rows + 1) * sizeof(int64_t));
      if (!ends) return std::unexpected(ends.error());
      if (ends->front() != 0 || ends->back() != static_cast<int64_t>(chars->size()) ||
          !std::ranges::is_sorted(*ends)) {
        return Error::Invalid("Spilled string offsets do not fit their chars");
      }
      values = Buffer::From(std::move(*chars));
      offsets = Buffer::From(std::move(*ends));
    } else {
      auto part =
          ReadPart<std::vector<StorageOf<T>>>(buffer, offset, num_rows * sizeof(StorageOf<T>));
      if (!part) return std::unexpected(part.error());
      values = Buffer::From(std::move(*part));
      auto no_offsets = ReadPart<std::vector<int64_t>>(buffer, offset, 0);
      if (!no_offsets) return std::unexpected(no_offsets.error());
    }
    return std::make_shared<const ColumnVector>(
        ColumnVector::Key{}, type, num_rows, *null_count, false,
        validity->empty() ? Buffer{} : Buffer::From(std::move(*validity)), std::move(values),
        std::move(offsets));
  });
}

}  // namespace quest::columnar
