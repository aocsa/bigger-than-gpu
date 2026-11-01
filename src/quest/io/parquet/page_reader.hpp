// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "quest/columnar/types.hpp"
#include "quest/io/parquet/format.hpp"
#include "quest/common/result.hpp"
#include "quest/io/parquet/encoding.hpp"
#include "quest/io/parquet/thrift_decoder.hpp"

namespace quest::io::parquet {


// =============================================================================
// Page Header Structure
// =============================================================================

/// @brief Page header information parsed from Thrift encoding.
struct PageHeader {
  PageType type = PageType::kDataPage;
  int32_t uncompressed_page_size = 0;
  int32_t compressed_page_size = 0;
  int32_t crc = 0;

  int32_t num_values = 0;
  Encoding encoding = Encoding::kPlain;
  Encoding definition_level_encoding = Encoding::kRle;
  Encoding repetition_level_encoding = Encoding::kRle;

  int32_t num_nulls = 0;
  int32_t num_rows = 0;
  int32_t definition_levels_byte_length = 0;
  int32_t repetition_levels_byte_length = 0;
  bool is_compressed = true;

  int32_t num_dict_values = 0;
  Encoding dict_encoding = Encoding::kPlain;
};

// =============================================================================
// Page Header Parser
// =============================================================================

class PageHeaderParser {
 public:
  [[nodiscard]] static Result<PageHeader> Parse(std::span<const uint8_t> data,
                                                 size_t& bytes_read) {
    ThriftDecoder decoder(data);
    auto result = ParsePageHeader(decoder);
    if (result.has_value()) {
      bytes_read = decoder.position();
    }
    return result;
  }

 private:
  [[nodiscard]] static Result<PageHeader> ParsePageHeader(
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

  [[nodiscard]] static Status ParseDataPageHeader(ThriftDecoder& decoder,
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

  [[nodiscard]] static Status ParseDataPageHeaderV2(ThriftDecoder& decoder,
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

  [[nodiscard]] static Status ParseDictionaryPageHeader(ThriftDecoder& decoder,
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
};

// =============================================================================
// Page Reader
// =============================================================================

/// Iterates through pages in a column chunk, parsing headers and exposing page data.
class PageReader {
 public:
  explicit PageReader(std::span<const uint8_t> data) : data_(data), pos_(0) {}

  [[nodiscard]] bool HasNextPage() const noexcept { return pos_ < data_.size(); }

  [[nodiscard]] Result<PageHeader> ReadPageHeader() {
    size_t bytes_read = 0;
    auto result = PageHeaderParser::Parse(data_.subspan(pos_), bytes_read);
    if (!result.has_value()) {
      return result;
    }
    pos_ += bytes_read;
    current_header_ = result.value();
    return result;
  }

  [[nodiscard]] Result<std::span<const uint8_t>> GetPageData() {
    size_t page_size = current_header_.compressed_page_size;
    if (pos_ + page_size > data_.size()) {
      return std::unexpected(
          Error::PageDecodeError("Page data exceeds buffer size"));
    }
    auto result = data_.subspan(pos_, page_size);
    pos_ += page_size;
    return result;
  }

  [[nodiscard]] const PageHeader& current_header() const noexcept {
    return current_header_;
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_;
  PageHeader current_header_;
};

}  // namespace quest::io::parquet
