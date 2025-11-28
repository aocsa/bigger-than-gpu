// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_PAGE_READER_HPP
#define PARQUET_READER_PAGE_READER_HPP

/// @file page_reader.hpp
/// @brief Page header parsing and page reading for Parquet column chunks.
///
/// This file provides the PageHeader structure and PageReader class for
/// reading and parsing pages from Parquet column chunks.

#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "encoding.hpp"
#include "quest/result.hpp"
#include "thrift_decoder.hpp"
#include "types.hpp"

namespace quest::datasource {

// =============================================================================
// Page Header Structure
// =============================================================================

/// @brief Page header information parsed from Thrift encoding.
struct PageHeader {
  PageType type = PageType::kDataPage;
  int32_t uncompressed_page_size = 0;
  int32_t compressed_page_size = 0;
  int32_t crc = 0;

  // Data page specific fields.
  int32_t num_values = 0;
  Encoding encoding = Encoding::kPlain;
  Encoding definition_level_encoding = Encoding::kRle;
  Encoding repetition_level_encoding = Encoding::kRle;

  // Data page v2 specific fields.
  int32_t num_nulls = 0;
  int32_t num_rows = 0;
  int32_t definition_levels_byte_length = 0;
  int32_t repetition_levels_byte_length = 0;
  bool is_compressed = true;

  // Dictionary page specific fields.
  int32_t num_dict_values = 0;
  Encoding dict_encoding = Encoding::kPlain;
};

// =============================================================================
// Page Header Parser
// =============================================================================

/// @brief Parser for Thrift-encoded page headers.
class PageHeaderParser {
 public:
  /// @brief Parses a page header from a byte span.
  /// @param data The byte span containing the Thrift-encoded header.
  /// @param[out] bytes_read The number of bytes consumed by the header.
  /// @return The parsed PageHeader, or decode error.
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
          if (!status.ok()) {
            return std::unexpected(status);
          }
          break;
        }
        case 6: {  // index_page_header
          auto status = decoder.SkipStruct();
          if (!status.ok()) {
            return std::unexpected(status);
          }
          break;
        }
        case 7: {  // dictionary_page_header
          auto status = ParseDictionaryPageHeader(decoder, header);
          if (!status.ok()) {
            return std::unexpected(status);
          }
          break;
        }
        case 8: {  // data_page_header_v2
          auto status = ParseDataPageHeaderV2(decoder, header);
          if (!status.ok()) {
            return std::unexpected(status);
          }
          break;
        }
        default: {
          auto status = decoder.SkipField(field->type);
          if (!status.ok()) {
            return std::unexpected(status);
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
        return field_result.error();
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
            return result.error();
          }
          header.num_values = result.value();
          break;
        }
        case 2: {  // encoding
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.encoding = static_cast<Encoding>(result.value());
          break;
        }
        case 3: {  // definition_level_encoding
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.definition_level_encoding =
              static_cast<Encoding>(result.value());
          break;
        }
        case 4: {  // repetition_level_encoding
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.repetition_level_encoding =
              static_cast<Encoding>(result.value());
          break;
        }
        default: {
          auto status = decoder.SkipField(field->type);
          if (!status.ok()) {
            return status;
          }
          break;
        }
      }
    }
    return Status::OK();
  }

  [[nodiscard]] static Status ParseDataPageHeaderV2(ThriftDecoder& decoder,
                                                     PageHeader& header) {
    int16_t last_field_id = 0;

    while (true) {
      auto field_result = decoder.ReadFieldHeader(last_field_id);
      if (!field_result.has_value()) {
        return field_result.error();
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
            return result.error();
          }
          header.num_values = result.value();
          break;
        }
        case 2: {  // num_nulls
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.num_nulls = result.value();
          break;
        }
        case 3: {  // num_rows
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.num_rows = result.value();
          break;
        }
        case 4: {  // encoding
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.encoding = static_cast<Encoding>(result.value());
          break;
        }
        case 5: {  // definition_levels_byte_length
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.definition_levels_byte_length = result.value();
          break;
        }
        case 6: {  // repetition_levels_byte_length
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
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
          if (!status.ok()) {
            return status;
          }
          break;
        }
      }
    }
    return Status::OK();
  }

  [[nodiscard]] static Status ParseDictionaryPageHeader(ThriftDecoder& decoder,
                                                         PageHeader& header) {
    int16_t last_field_id = 0;

    while (true) {
      auto field_result = decoder.ReadFieldHeader(last_field_id);
      if (!field_result.has_value()) {
        return field_result.error();
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
            return result.error();
          }
          header.num_dict_values = result.value();
          break;
        }
        case 2: {  // encoding
          auto result = decoder.ReadI32();
          if (!result.has_value()) {
            return result.error();
          }
          header.dict_encoding = static_cast<Encoding>(result.value());
          break;
        }
        default: {
          auto status = decoder.SkipField(field->type);
          if (!status.ok()) {
            return status;
          }
          break;
        }
      }
    }
    return Status::OK();
  }
};

// =============================================================================
// Page Reader
// =============================================================================

/// @brief Page reader that handles reading and decoding pages from a column chunk.
///
/// The PageReader iterates through pages in a column chunk, parsing headers
/// and providing access to page data.
class PageReader {
 public:
  /// @brief Constructs a page reader for a column chunk.
  /// @param data The raw bytes of the column chunk.
  /// @param type The physical type of the column.
  /// @param max_definition_level Maximum definition level for the column.
  /// @param max_repetition_level Maximum repetition level for the column.
  PageReader(std::span<const uint8_t> data, PhysicalType type,
             int max_definition_level, int max_repetition_level)
      : data_(data),
        pos_(0),
        physical_type_(type),
        max_definition_level_(max_definition_level),
        max_repetition_level_(max_repetition_level) {}

  /// @brief Checks if there are more pages to read.
  [[nodiscard]] bool HasNextPage() const noexcept { return pos_ < data_.size(); }

  /// @brief Reads the next page header.
  /// @return The parsed PageHeader, or decode error.
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

  /// @brief Gets the data for the current page.
  /// @return The page data span, or error if page exceeds buffer.
  [[nodiscard]] Result<std::span<const uint8_t>> GetPageData() {
    size_t page_size = current_header_.compressed_page_size;
    if (pos_ + page_size > data_.size()) {
      return std::unexpected(
          Status::PageDecodeError("Page data exceeds buffer size"));
    }
    auto result = data_.subspan(pos_, page_size);
    pos_ += page_size;
    return result;
  }

  /// @brief Returns the current page header.
  [[nodiscard]] const PageHeader& current_header() const noexcept {
    return current_header_;
  }

 private:
  std::span<const uint8_t> data_;
  size_t pos_;
  PhysicalType physical_type_;
  int max_definition_level_;
  int max_repetition_level_;
  PageHeader current_header_;
};

// =============================================================================
// Definition Level Decoding
// =============================================================================

/// @brief Decodes definition levels from page data using RLE/bit-packing.
/// @param data The encoded definition levels data.
/// @param max_level The maximum definition level.
/// @param num_values The number of values to decode.
/// @return The decoded definition levels, or decode error.
[[nodiscard]] inline Result<std::vector<int32_t>> DecodeDefinitionLevels(
    std::span<const uint8_t> data, int max_level, size_t num_values) {
  std::vector<int32_t> levels(num_values);

  if (max_level == 0) {
    // All values are required, no nulls possible.
    std::fill(levels.begin(), levels.end(), 0);
    return levels;
  }

  // First 4 bytes contain the length of the RLE data.
  if (data.size() < 4) {
    return std::unexpected(
        Status::PageDecodeError("Not enough data for definition levels"));
  }
  uint32_t rle_length;
  std::memcpy(&rle_length, data.data(), 4);

  int bit_width = ComputeBitWidth(max_level);
  RleBitPackingDecoder decoder(bit_width);
  decoder.SetData(data.subspan(4, rle_length), num_values);
  auto status = decoder.ReadBatch(levels);
  if (!status.ok()) {
    return std::unexpected(status);
  }

  return levels;
}

}  // namespace quest::datasource

#endif  // PARQUET_READER_PAGE_READER_HPP
