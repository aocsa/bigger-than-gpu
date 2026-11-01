// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/io/parquet/thrift_decoder.hpp"

namespace quest::io::parquet {

Status ThriftDecoder::Skip(size_t count) noexcept {
  if (pos_ + count > data_.size()) {
    return Error::ThriftDecodeError("Unexpected end of data during skip");
  }
  pos_ += count;
  return {};
}

Result<uint8_t> ThriftDecoder::ReadByte() noexcept {
  if (pos_ >= data_.size()) {
    return std::unexpected(
        Error::ThriftDecodeError("Unexpected end of data reading byte"));
  }
  return data_[pos_++];
}

Result<int32_t> ThriftDecoder::ReadVarint32() noexcept {
  uint32_t result = 0;
  uint32_t shift = 0;

  while (true) {
    auto byte_result = ReadByte();
    if (!byte_result.has_value()) {
      return std::unexpected(byte_result.error());
    }
    uint8_t byte = byte_result.value();

    result |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      break;
    }

    shift += 7;
    if (shift > 28) {
      return std::unexpected(
          Error::ThriftDecodeError("Varint32 too long (exceeds 5 bytes)"));
    }
  }

  // Zigzag decode: convert unsigned to signed.
  return static_cast<int32_t>((result >> 1) ^ -(result & 1));
}

Result<int64_t> ThriftDecoder::ReadVarint64() noexcept {
  uint64_t result = 0;
  uint64_t shift = 0;

  while (true) {
    auto byte_result = ReadByte();
    if (!byte_result.has_value()) {
      return std::unexpected(byte_result.error());
    }
    uint8_t byte = byte_result.value();

    result |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      break;
    }

    shift += 7;
    if (shift > 63) {
      return std::unexpected(
          Error::ThriftDecodeError("Varint64 too long (exceeds 10 bytes)"));
    }
  }

  // Zigzag decode: convert unsigned to signed.
  return static_cast<int64_t>((result >> 1) ^ -(result & 1));
}

Result<uint32_t> ThriftDecoder::ReadUvarint32() noexcept {
  uint32_t result = 0;
  uint32_t shift = 0;

  while (true) {
    auto byte_result = ReadByte();
    if (!byte_result.has_value()) {
      return std::unexpected(byte_result.error());
    }
    uint8_t byte = byte_result.value();

    result |= static_cast<uint32_t>(byte & 0x7F) << shift;
    if ((byte & 0x80) == 0) {
      break;
    }

    shift += 7;
  }

  return result;
}

Result<int16_t> ThriftDecoder::ReadI16() noexcept {
  auto result = ReadVarint32();
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }
  return static_cast<int16_t>(result.value());
}

Result<std::string> ThriftDecoder::ReadString() noexcept {
  auto length_result = ReadUvarint32();
  if (!length_result.has_value()) {
    return std::unexpected(length_result.error());
  }
  uint32_t length = length_result.value();

  if (pos_ + length > data_.size()) {
    return Error::ThriftDecodeError(
        "String length exceeds remaining buffer");
  }

  std::string result(reinterpret_cast<const char*>(&data_[pos_]), length);
  pos_ += length;
  return result;
}

Result<std::vector<uint8_t>> ThriftDecoder::ReadBinary() noexcept {
  auto length_result = ReadUvarint32();
  if (!length_result.has_value()) {
    return std::unexpected(length_result.error());
  }
  uint32_t length = length_result.value();

  if (pos_ + length > data_.size()) {
    return Error::ThriftDecodeError(
        "Binary length exceeds remaining buffer");
  }

  std::vector<uint8_t> result(data_.begin() + pos_,
                              data_.begin() + pos_ + length);
  pos_ += length;
  return result;
}

Result<std::optional<ThriftDecoder::FieldHeader>> ThriftDecoder::ReadFieldHeader(
    int16_t last_field_id) noexcept {
  auto byte_result = ReadByte();
  if (!byte_result.has_value()) {
    return std::unexpected(byte_result.error());
  }
  uint8_t byte = byte_result.value();

  // Check for STOP field (end of struct).
  if (byte == 0) {
    return std::nullopt;
  }

  // Extract type from low nibble.
  ThriftType type = static_cast<ThriftType>(byte & 0x0F);
  int16_t field_id;

  // Extract field ID delta from high nibble.
  int16_t delta = (byte >> 4) & 0x0F;
  if (delta == 0) {
    // Full field ID follows.
    auto id_result = ReadI16();
    if (!id_result.has_value()) {
      return std::unexpected(id_result.error());
    }
    field_id = id_result.value();
  } else {
    // Delta-encoded field ID.
    field_id = last_field_id + delta;
  }

  return FieldHeader{field_id, type};
}

Result<ThriftDecoder::ListHeader> ThriftDecoder::ReadListHeader() noexcept {
  auto byte_result = ReadByte();
  if (!byte_result.has_value()) {
    return std::unexpected(byte_result.error());
  }
  uint8_t byte = byte_result.value();

  // Extract element type from low nibble.
  ThriftType elem_type = static_cast<ThriftType>(byte & 0x0F);

  // Extract size from high nibble (or read full varint if 0xF).
  uint32_t size = (byte >> 4) & 0x0F;
  if (size == 0x0F) {
    auto size_result = ReadUvarint32();
    if (!size_result.has_value()) {
      return std::unexpected(size_result.error());
    }
    size = size_result.value();
  }
  // Every compact-protocol element takes at least one byte; a larger count is corrupt and
  // would otherwise drive a huge reserve() in the caller.
  if (size > remaining()) {
    return Error::ThriftDecodeError("List size exceeds remaining buffer");
  }

  return ListHeader{elem_type, size};
}

Status ThriftDecoder::SkipField(ThriftType type) noexcept {
  switch (type) {
    case ThriftType::kBoolTrue:
    case ThriftType::kBoolFalse:
      // No data to skip for inline booleans.
      return {};

    case ThriftType::kByte: {
      auto status = Skip(1);
      if (!status.has_value()) {
        return status;
      }
      return {};
    }

    case ThriftType::kI16:
    case ThriftType::kI32: {
      auto result = ReadVarint32();
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      return {};
    }

    case ThriftType::kI64: {
      auto result = ReadVarint64();
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      return {};
    }

    case ThriftType::kDouble: {
      auto status = Skip(8);
      if (!status.has_value()) {
        return status;
      }
      return {};
    }

    case ThriftType::kBinary: {
      auto length_result = ReadUvarint32();
      if (!length_result.has_value()) {
        return std::unexpected(length_result.error());
      }
      auto status = Skip(length_result.value());
      if (!status.has_value()) {
        return status;
      }
      return {};
    }

    case ThriftType::kList:
    case ThriftType::kSet: {
      auto header_result = ReadListHeader();
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();

      // Skip each element.
      for (uint32_t i = 0; i < header.size; ++i) {
        auto status = SkipField(header.element_type);
        if (!status.has_value()) {
          return status;
        }
      }
      return {};
    }

    case ThriftType::kMap: {
      auto size_result = ReadUvarint32();
      if (!size_result.has_value()) {
        return std::unexpected(size_result.error());
      }
      uint32_t size = size_result.value();

      if (size > 0) {
        auto types_result = ReadByte();
        if (!types_result.has_value()) {
          return std::unexpected(types_result.error());
        }
        uint8_t types = types_result.value();

        ThriftType key_type = static_cast<ThriftType>((types >> 4) & 0x0F);
        ThriftType val_type = static_cast<ThriftType>(types & 0x0F);

        // Skip each key-value pair.
        for (uint32_t i = 0; i < size; ++i) {
          auto key_status = SkipField(key_type);
          if (!key_status.has_value()) {
            return key_status;
          }
          auto val_status = SkipField(val_type);
          if (!val_status.has_value()) {
            return val_status;
          }
        }
      }
      return {};
    }

    case ThriftType::kStruct:
      return SkipStruct();

    default:
      return Error::ThriftDecodeError("Unknown Thrift type to skip");
  }
}

Status ThriftDecoder::SkipStruct() noexcept {
  int16_t last_field_id = 0;

  while (true) {
    auto header_result = ReadFieldHeader(last_field_id);
    if (!header_result.has_value()) {
      return std::unexpected(header_result.error());
    }

    auto header = header_result.value();
    if (!header) {
      // STOP field encountered.
      break;
    }

    last_field_id = header->field_id;
    auto status = SkipField(header->type);
    if (!status.has_value()) {
      return status;
    }
  }

  return {};
}

}  // namespace quest::io::parquet
