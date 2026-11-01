// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/io/parquet/page_reader.hpp"

#include "quest/io/parquet/bytes.hpp"
#include "quest/io/parquet/thrift_decoder.hpp"

namespace quest::io::parquet {

namespace {

Status ReadDataPageHeader(ThriftDecoder& d, PageHeader& header) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, header.num_values);
      case 2: return Read(d, header.encoding);
      case 3: return Read(d, header.definition_level_encoding);
      case 4: return Read(d, header.repetition_level_encoding);
      default: return d.SkipField(type);
    }
  });
}

Status ReadDataPageHeaderV2(ThriftDecoder& d, PageHeader& header) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, header.num_values);
      case 2: return Read(d, header.num_nulls);
      case 3: return Read(d, header.num_rows);
      case 4: return Read(d, header.encoding);
      case 5: return Read(d, header.definition_levels_byte_length);
      case 6: return Read(d, header.repetition_levels_byte_length);
      case 7:  // is_compressed: compact thrift carries a bool in the field header
        header.is_compressed = type == ThriftType::kBoolTrue;
        return {};
      default: return d.SkipField(type);
    }
  });
}

Status ReadDictionaryPageHeader(ThriftDecoder& d, PageHeader& header) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, header.num_dict_values);
      case 2: return Read(d, header.dict_encoding);
      default: return d.SkipField(type);
    }
  });
}

Status Read(ThriftDecoder& d, PageHeader& header) {
  return ReadStruct(d, [&](int16_t id, ThriftType type) -> Status {
    switch (id) {
      case 1: return Read(d, header.type);
      case 2: return Read(d, header.uncompressed_page_size);
      case 3: return Read(d, header.compressed_page_size);
      case 4: return Read(d, header.crc);
      case 5: return ReadDataPageHeader(d, header);
      case 6: return d.SkipStruct();  // index_page_header
      case 7: return ReadDictionaryPageHeader(d, header);
      case 8: return ReadDataPageHeaderV2(d, header);
      default: return d.SkipField(type);
    }
  });
}

}  // namespace

Result<PageHeader> PageHeaderParser::Parse(std::span<const uint8_t> data, size_t& bytes_read) {
  ThriftDecoder decoder(data);
  PageHeader header;
  return Read(decoder, header).transform([&] {
    bytes_read = decoder.position();
    return header;
  });
}

Result<PageHeader> PageReader::ReadPageHeader() {
  size_t bytes_read = 0;
  auto result = PageHeaderParser::Parse(data_.subspan(pos_), bytes_read);
  if (!result.has_value()) {
    return result;
  }
  pos_ += bytes_read;
  current_header_ = result.value();
  return result;
}

Result<std::span<const uint8_t>> PageReader::GetPageData() {
  if (current_header_.compressed_page_size < 0) {
    return Error::PageDecodeError("Negative compressed page size");
  }
  auto result = Slice(data_, pos_, static_cast<size_t>(current_header_.compressed_page_size));
  if (!result) {
    return std::unexpected(Error::PageDecodeError("Page data exceeds buffer size"));
  }
  pos_ += result->size();
  return result;
}

}  // namespace quest::io::parquet
