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

namespace {

// An unsigned LEB128 varint of a U: at most ceil(bits / 7) bytes, else @p too_long.
template <typename U>
Result<U> ReadUvarint(ThriftDecoder& decoder, const char* too_long) {
  U result = 0;
  unsigned shift = 0;
  while (true) {
    auto byte = decoder.ReadByte();
    if (!byte) {
      return std::unexpected(std::move(byte).error());
    }
    result |= static_cast<U>(*byte & 0x7F) << shift;
    if ((*byte & 0x80) == 0) {
      return result;
    }
    shift += 7;
    if (shift >= sizeof(U) * 8) {
      return Error::ThriftDecodeError(too_long);
    }
  }
}

// Zigzag decoding: 0, 1, 2, 3, ... are 0, -1, 1, -2, ...
template <typename U>
std::make_signed_t<U> Unzigzag(U value) {
  return static_cast<std::make_signed_t<U>>((value >> 1) ^ -(value & 1));
}

}  // namespace

Result<int32_t> ThriftDecoder::ReadVarint32() noexcept {
  return ReadUvarint<uint32_t>(*this, "Varint32 too long (exceeds 5 bytes)")
      .transform(Unzigzag<uint32_t>);
}

Result<int64_t> ThriftDecoder::ReadVarint64() noexcept {
  return ReadUvarint<uint64_t>(*this, "Varint64 too long (exceeds 10 bytes)")
      .transform(Unzigzag<uint64_t>);
}

Result<uint32_t> ThriftDecoder::ReadUvarint32() noexcept {
  return ReadUvarint<uint32_t>(*this, "Uvarint32 too long (exceeds 5 bytes)");
}

Result<int16_t> ThriftDecoder::ReadI16() noexcept {
  return ReadVarint32().transform([](int32_t value) { return static_cast<int16_t>(value); });
}

Result<std::span<const uint8_t>> ThriftDecoder::ReadLengthPrefixed(
    const char* too_long) noexcept {
  auto length = ReadUvarint32();
  if (!length) {
    return std::unexpected(std::move(length).error());
  }
  if (pos_ + *length > data_.size()) {
    return Error::ThriftDecodeError(too_long);
  }
  const auto bytes = data_.subspan(pos_, *length);
  pos_ += *length;
  return bytes;
}

Result<std::string> ThriftDecoder::ReadString() noexcept {
  return ReadLengthPrefixed("String length exceeds remaining buffer")
      .transform([](std::span<const uint8_t> bytes) {
        return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
      });
}

Result<std::vector<uint8_t>> ThriftDecoder::ReadBinary() noexcept {
  return ReadLengthPrefixed("Binary length exceeds remaining buffer")
      .transform([](std::span<const uint8_t> bytes) {
        return std::vector<uint8_t>(bytes.begin(), bytes.end());
      });
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
      return {};  // The value is the field header's type.
    case ThriftType::kByte:
      return Skip(1);
    case ThriftType::kI16:
    case ThriftType::kI32:
      return ReadVarint32().transform([](int32_t) {});
    case ThriftType::kI64:
      return ReadVarint64().transform([](int64_t) {});
    case ThriftType::kDouble:
      return Skip(8);
    case ThriftType::kBinary:
      return ReadUvarint32().and_then([this](uint32_t length) { return Skip(length); });
    case ThriftType::kList:
    case ThriftType::kSet: {
      auto header = ReadListHeader();
      if (!header) {
        return std::unexpected(std::move(header).error());
      }
      for (uint32_t i = 0; i < header->size; ++i) {
        if (auto status = SkipField(header->element_type); !status) {
          return status;
        }
      }
      return {};
    }
    case ThriftType::kMap: {
      auto size = ReadUvarint32();
      if (!size) {
        return std::unexpected(std::move(size).error());
      }
      if (*size == 0) {
        return {};
      }
      auto types = ReadByte();
      if (!types) {
        return std::unexpected(std::move(types).error());
      }
      // As for lists: a larger count is corrupt, and bool entries read no bytes, so nothing
      // else would stop the loop below.
      if (*size > remaining()) {
        return Error::ThriftDecodeError("Map size exceeds remaining buffer");
      }
      const auto key_type = static_cast<ThriftType>((*types >> 4) & 0x0F);
      const auto value_type = static_cast<ThriftType>(*types & 0x0F);
      for (uint32_t i = 0; i < *size; ++i) {
        auto status = SkipField(key_type).and_then([&] { return SkipField(value_type); });
        if (!status) {
          return status;
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
  return ReadStruct(*this, [this](int16_t, ThriftType type) { return SkipField(type); });
}

}  // namespace quest::io::parquet
