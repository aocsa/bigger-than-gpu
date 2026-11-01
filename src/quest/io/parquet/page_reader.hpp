// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <cstdint>
#include <span>

#include "quest/io/parquet/format.hpp"
#include "quest/common/result.hpp"

namespace quest::io::parquet {

class ThriftDecoder;

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
                                                 size_t& bytes_read);

 private:
  [[nodiscard]] static Result<PageHeader> ParsePageHeader(
      ThriftDecoder& decoder);

  [[nodiscard]] static Status ParseDataPageHeader(ThriftDecoder& decoder,
                                                   PageHeader& header);

  [[nodiscard]] static Status ParseDataPageHeaderV2(ThriftDecoder& decoder,
                                                     PageHeader& header);

  [[nodiscard]] static Status ParseDictionaryPageHeader(ThriftDecoder& decoder,
                                                         PageHeader& header);
};

// =============================================================================
// Page Reader
// =============================================================================

/// Iterates through pages in a column chunk, parsing headers and exposing page data.
class PageReader {
 public:
  explicit PageReader(std::span<const uint8_t> data) : data_(data), pos_(0) {}

  [[nodiscard]] bool HasNextPage() const noexcept { return pos_ < data_.size(); }

  [[nodiscard]] Result<PageHeader> ReadPageHeader();

  [[nodiscard]] Result<std::span<const uint8_t>> GetPageData();

 private:
  std::span<const uint8_t> data_;
  size_t pos_;
  PageHeader current_header_;
};

}  // namespace quest::io::parquet
