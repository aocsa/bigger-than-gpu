// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/io/parquet/page_reader.hpp"

#include "quest/io/parquet/bytes.hpp"
#include "quest/io/parquet/thrift_decoder.hpp"

namespace quest::io::parquet {

Result<PageHeader> PageHeaderParser::Parse(std::span<const uint8_t> data,
                                           size_t& bytes_read) {
  ThriftDecoder decoder(data);
  auto result = ParsePageHeader(decoder);
  if (result.has_value()) {
    bytes_read = decoder.position();
  }
  return result;
}

Result<PageHeader> PageHeaderParser::ParsePageHeader(
    ThriftDecoder& decoder) {
  PageHeader header;
  int16_t last_field_id = 0;

  while (true) {
    auto field_result = decoder.ReadFieldHeader(last_field_id);
    if (!field_result.has_value()) {
      return std::unexpected(field_result.error());
    }
    auto field = field_result.value();
    if (!field) {
      break;
    }
    last_field_id = field->field_id;

    switch (field->field_id) {
      case 1: {  // type
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.type = static_cast<PageType>(result.value());
        break;
      }
      case 2: {  // uncompressed_page_size
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.uncompressed_page_size = result.value();
        break;
      }
      case 3: {  // compressed_page_size
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.compressed_page_size = result.value();
        break;
      }
      case 4: {  // crc
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.crc = result.value();
        break;
      }
      case 5: {  // data_page_header
        auto status = ParseDataPageHeader(decoder, header);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      case 6: {  // index_page_header
        auto status = decoder.SkipStruct();
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      case 7: {  // dictionary_page_header
        auto status = ParseDictionaryPageHeader(decoder, header);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      case 8: {  // data_page_header_v2
        auto status = ParseDataPageHeaderV2(decoder, header);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      default: {
        auto status = decoder.SkipField(field->type);
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
    }
  }
  return header;
}

Status PageHeaderParser::ParseDataPageHeader(ThriftDecoder& decoder,
                                             PageHeader& header) {
  int16_t last_field_id = 0;

  while (true) {
    auto field_result = decoder.ReadFieldHeader(last_field_id);
    if (!field_result.has_value()) {
      return std::unexpected(field_result.error());
    }
    auto field = field_result.value();
    if (!field) {
      break;
    }
    last_field_id = field->field_id;

    switch (field->field_id) {
      case 1: {  // num_values
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.num_values = result.value();
        break;
      }
      case 2: {  // encoding
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.encoding = static_cast<Encoding>(result.value());
        break;
      }
      case 3: {  // definition_level_encoding
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.definition_level_encoding =
            static_cast<Encoding>(result.value());
        break;
      }
      case 4: {  // repetition_level_encoding
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.repetition_level_encoding =
            static_cast<Encoding>(result.value());
        break;
      }
      default: {
        auto status = decoder.SkipField(field->type);
        if (!status.has_value()) {
          return status;
        }
        break;
      }
    }
  }
  return {};
}

Status PageHeaderParser::ParseDataPageHeaderV2(ThriftDecoder& decoder,
                                               PageHeader& header) {
  int16_t last_field_id = 0;

  while (true) {
    auto field_result = decoder.ReadFieldHeader(last_field_id);
    if (!field_result.has_value()) {
      return std::unexpected(field_result.error());
    }
    auto field = field_result.value();
    if (!field) {
      break;
    }
    last_field_id = field->field_id;

    switch (field->field_id) {
      case 1: {  // num_values
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.num_values = result.value();
        break;
      }
      case 2: {  // num_nulls
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.num_nulls = result.value();
        break;
      }
      case 3: {  // num_rows
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.num_rows = result.value();
        break;
      }
      case 4: {  // encoding
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.encoding = static_cast<Encoding>(result.value());
        break;
      }
      case 5: {  // definition_levels_byte_length
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.definition_levels_byte_length = result.value();
        break;
      }
      case 6: {  // repetition_levels_byte_length
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.repetition_levels_byte_length = result.value();
        break;
      }
      case 7: {  // is_compressed
        header.is_compressed = (field->type == ThriftType::kBoolTrue);
        break;
      }
      default: {
        auto status = decoder.SkipField(field->type);
        if (!status.has_value()) {
          return status;
        }
        break;
      }
    }
  }
  return {};
}

Status PageHeaderParser::ParseDictionaryPageHeader(ThriftDecoder& decoder,
                                                   PageHeader& header) {
  int16_t last_field_id = 0;

  while (true) {
    auto field_result = decoder.ReadFieldHeader(last_field_id);
    if (!field_result.has_value()) {
      return std::unexpected(field_result.error());
    }
    auto field = field_result.value();
    if (!field) {
      break;
    }
    last_field_id = field->field_id;

    switch (field->field_id) {
      case 1: {  // num_values
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.num_dict_values = result.value();
        break;
      }
      case 2: {  // encoding
        auto result = decoder.ReadI32();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        header.dict_encoding = static_cast<Encoding>(result.value());
        break;
      }
      default: {
        auto status = decoder.SkipField(field->type);
        if (!status.has_value()) {
          return status;
        }
        break;
      }
    }
  }
  return {};
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
