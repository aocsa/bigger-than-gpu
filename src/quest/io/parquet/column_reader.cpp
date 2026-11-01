// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/io/parquet/column_reader.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "quest/io/parquet/bytes.hpp"
#include "quest/io/parquet/encoding.hpp"

namespace quest::io::parquet {

namespace {

/// Iterates over def_levels (or num_values if empty), calling read_fn/append_fn
/// for present values and append_null_fn for nulls.
/// @tparam ReadFn  () -> Result<T>
/// @tparam AppendFn  (T&&) -> void
/// @tparam AppendNullFn  () -> void
template <typename ReadFn, typename AppendFn, typename AppendNullFn>
[[nodiscard]] Status ProcessWithDefLevels(
    size_t num_values, const std::vector<int32_t>& def_levels,
    ReadFn&& read_fn, AppendFn&& append_fn, AppendNullFn&& append_null_fn) {
  if (def_levels.empty()) {
    for (size_t i = 0; i < num_values; ++i) {
      auto result = read_fn();
      if (!result.has_value()) {
        return std::unexpected(result.error());
      }
      append_fn(std::move(result.value()));
    }
  } else {
    for (size_t i = 0; i < def_levels.size(); ++i) {
      if (def_levels[i] == 1) {
        auto result = read_fn();
        if (!result.has_value()) {
          return std::unexpected(result.error());
        }
        append_fn(std::move(result.value()));
      } else {
        append_null_fn();
      }
    }
  }
  return {};
}

/// @brief Unscaled DECIMAL values as Double. Division keeps 7 / 100 equal to the literal 0.07,
/// which multiplying by 0.01 does not.
template <typename T>
[[nodiscard]] std::shared_ptr<ColumnVector> DecimalToDouble(const TypedColumnVector<T>& unscaled,
                                                            int32_t scale) {
  const double divisor = std::pow(10.0, scale);
  auto column = std::make_shared<TypedColumnVector<double>>(columnar::DoubleType{}, unscaled.size());
  for (size_t i = 0; i < unscaled.size(); ++i) {
    if (unscaled.IsNull(i)) {
      column->AppendNull();
    } else {
      column->Append(static_cast<double>(unscaled.GetTypedValue(i)) / divisor);
    }
  }
  return column;
}

}  // namespace

Result<std::shared_ptr<ColumnVector>> ColumnReader::ReadAll() {
  if (column_meta_.num_values < 0) {
    return Error::ColumnDecodeError("Negative column chunk value count");
  }
  size_t total_values = column_meta_.num_values;
  // A DECIMAL column decodes as its physical integer type and converts to Double at the end.
  auto column = CreateColumnVector(
      field_.decimal_scale ? PhysicalTypeToDataType(column_meta_.type) : field_.data_type,
      total_values);

  while (page_reader_.HasNextPage()) {
    auto header_result = page_reader_.ReadPageHeader();
    if (!header_result.has_value()) {
      return std::unexpected(header_result.error());
    }
    auto header = header_result.value();
    if (header.num_values < 0 || header.num_dict_values < 0 ||
        header.uncompressed_page_size < 0) {
      return Error::PageDecodeError("Negative count in page header");
    }

    auto page_data_result = page_reader_.GetPageData();
    if (!page_data_result.has_value()) {
      return std::unexpected(page_data_result.error());
    }
    auto compressed_data = page_data_result.value();

    // Decompress page data if needed.
    auto decompressed_result = DecompressPageData(header, compressed_data);
    if (!decompressed_result.has_value()) {
      return std::unexpected(decompressed_result.error());
    }
    auto page_data = decompressed_result.value();

    switch (header.type) {
      case PageType::kDictionaryPage:
        ReadDictionaryPage(header, page_data);
        break;
      case PageType::kDataPage: {
        auto status = ReadDataPage(header, page_data, column.get());
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      case PageType::kDataPageV2: {
        auto status = ReadDataPageV2(header, page_data, column.get());
        if (!status.has_value()) {
          return std::unexpected(status.error());
        }
        break;
      }
      default:
        // Skip unknown page types.
        break;
    }
  }

  if (field_.decimal_scale) {
    if (const auto* i64 = dynamic_cast<const TypedColumnVector<int64_t>*>(column.get())) {
      return DecimalToDouble(*i64, *field_.decimal_scale);
    }
    // decimal_scale is set only for INT32 and INT64 columns (metadata.cpp IsIntegerDecimal).
    return DecimalToDouble(static_cast<const TypedColumnVector<int32_t>&>(*column),
                           *field_.decimal_scale);
  }
  return column;
}

Result<std::span<const uint8_t>> ColumnReader::DecompressPageData(
    const PageHeader& header, std::span<const uint8_t> compressed_data) {
  // Equal compressed and uncompressed sizes do not mean "stored raw": a small Snappy page can
  // be the same size either way (testdata/snappy_dictionary_page.parquet).
  bool needs_decompression = codec_->type() != CompressionType::kUncompressed;

  // For DataPageV2, check the is_compressed flag.
  if (header.type == PageType::kDataPageV2 && !header.is_compressed) {
    needs_decompression = false;
  }

  if (!needs_decompression) {
    return Result<std::span<const uint8_t>>{
        std::in_place, compressed_data.data(), compressed_data.size()};
  }

  auto result =
      codec_->Decompress(compressed_data, header.uncompressed_page_size);
  if (!result.has_value()) {
    return std::unexpected(result.error());
  }

  decompression_buffer_ = std::move(result.value());
  return Result<std::span<const uint8_t>>{
      std::in_place, decompression_buffer_.data(), decompression_buffer_.size()};
}

void ColumnReader::ReadDictionaryPage(const PageHeader& header,
                                      std::span<const uint8_t> data) {
  // Store dictionary for later use with dictionary-encoded pages.
  dictionary_data_ = std::vector<uint8_t>(data.begin(), data.end());
  dict_num_values_ = header.num_dict_values;
  has_dictionary_ = true;
}

Status ColumnReader::ReadDataPage(const PageHeader& header,
                                  std::span<const uint8_t> data,
                                  ColumnVector* column) {
  size_t pos = 0;
  int max_def_level = field_.IsNullable() ? 1 : 0;

  std::vector<int32_t> def_levels;
  if (max_def_level > 0) {
    // Definition levels are prefixed with their byte length.
    auto def_level_length = LoadLE<uint32_t>(data, pos);
    if (!def_level_length) {
      return std::unexpected(std::move(def_level_length).error());
    }
    pos += 4;

    auto def_data = Slice(data, pos, *def_level_length);
    if (!def_data) {
      return std::unexpected(std::move(def_data).error());
    }
    auto levels_result =
        DecodeDefinitionLevelsRle(*def_data, max_def_level, header.num_values);
    if (!levels_result.has_value()) {
      return std::unexpected(levels_result.error());
    }
    def_levels = std::move(levels_result.value());
    pos += *def_level_length;
  }

  auto values_data = data.subspan(pos);
  return DecodeValues(header.encoding, values_data, header.num_values,
                      def_levels, column);
}

Status ColumnReader::ReadDataPageV2(const PageHeader& header,
                                    std::span<const uint8_t> data,
                                    ColumnVector* column) {
  if (header.repetition_levels_byte_length < 0 || header.definition_levels_byte_length < 0 ||
      header.num_nulls < 0 || header.num_nulls > header.num_values) {
    return Error::PageDecodeError("Invalid data page V2 header");
  }
  // Skip repetition levels (always 0 for flat schema).
  size_t pos = static_cast<size_t>(header.repetition_levels_byte_length);

  // Read definition levels.
  std::vector<int32_t> def_levels;
  int max_def_level = field_.IsNullable() ? 1 : 0;
  if (max_def_level > 0 && header.definition_levels_byte_length > 0) {
    auto def_data = Slice(data, pos, header.definition_levels_byte_length);
    if (!def_data) {
      return std::unexpected(std::move(def_data).error());
    }
    auto levels_result =
        DecodeDefinitionLevelsRle(*def_data, max_def_level, header.num_values);
    if (!levels_result.has_value()) {
      return std::unexpected(levels_result.error());
    }
    def_levels = std::move(levels_result.value());
    pos += header.definition_levels_byte_length;
  }

  if (pos > data.size()) {
    return Error::PageDecodeError("Data page V2 levels exceed page size");
  }
  auto values_data = data.subspan(pos);
  // V2 pages store non-null value count.
  size_t num_non_null = header.num_values - header.num_nulls;
  return DecodeValues(header.encoding, values_data, num_non_null, def_levels,
                      column);
}

Result<std::vector<int32_t>> ColumnReader::DecodeDefinitionLevelsRle(
    std::span<const uint8_t> data, int max_level, size_t num_values) {
  std::vector<int32_t> levels(num_values);
  int bit_width = ComputeBitWidth(max_level);
  RleBitPackingDecoder decoder(bit_width);
  decoder.SetData(data, num_values);
  auto status = decoder.ReadBatch(levels);
  if (!status.has_value()) {
    return std::unexpected(status.error());
  }
  return levels;
}

Status ColumnReader::DecodeValues(Encoding encoding,
                                  std::span<const uint8_t> data,
                                  size_t num_values,
                                  const std::vector<int32_t>& def_levels,
                                  ColumnVector* column) {
  bool use_dictionary = (encoding == Encoding::kPlainDictionary ||
                         encoding == Encoding::kRleDictionary);

  if (use_dictionary && !has_dictionary_) {
    return Error::ColumnDecodeError(
        "Dictionary encoding but no dictionary page found");
  }

  switch (column_meta_.type) {
    case PhysicalType::kBoolean:
      return DecodeBooleanValues(encoding, data, num_values, def_levels,
                                 column);
    case PhysicalType::kInt32:
      if (std::holds_alternative<columnar::Date32Type>(field_.data_type)) {
        return DecodeFixedWidthValues<columnar::Date32>(encoding, data, num_values, def_levels,
                                                        column);
      }
      return DecodeFixedWidthValues<int32_t>(encoding, data, num_values,
                                             def_levels, column);
    case PhysicalType::kInt64:
      return DecodeFixedWidthValues<int64_t>(encoding, data, num_values,
                                             def_levels, column);
    case PhysicalType::kFloat:
      return DecodeFixedWidthValues<float>(encoding, data, num_values,
                                           def_levels, column);
    case PhysicalType::kDouble:
      return DecodeFixedWidthValues<double>(encoding, data, num_values,
                                            def_levels, column);
    case PhysicalType::kByteArray:
      return DecodeBinaryValues(encoding, data, num_values, def_levels,
                                column);
    case PhysicalType::kFixedLenByteArray:
      return DecodeFixedLenByteArrayValues(encoding, data, num_values,
                                           def_levels, column);
    case PhysicalType::kInt96:
      return DecodeInt96Values(encoding, data, num_values, def_levels,
                               column);
  }
  return {};
}

Status ColumnReader::DecodeBooleanValues(
    Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
    const std::vector<int32_t>& def_levels, ColumnVector* column) {
  auto* typed_col = dynamic_cast<TypedColumnVector<bool>*>(column);
  if (!typed_col) {
    return Error::TypeError("Column type mismatch for boolean");
  }

  PlainDecoder<bool> decoder;
  decoder.SetData(data);
  decoder.SetNumValues(num_values);

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&decoder]() { return decoder.ReadValue(); },
      [typed_col](bool val) { typed_col->Append(val); },
      [typed_col]() { typed_col->AppendNull(); });
}

template <typename T>
Status ColumnReader::DecodeFixedWidthValues(
    Encoding encoding, std::span<const uint8_t> data, size_t num_values,
    const std::vector<int32_t>& def_levels, ColumnVector* column) {
  auto* typed_col = dynamic_cast<TypedColumnVector<T>*>(column);
  if (!typed_col) {
    return Error::TypeError("Column type mismatch");
  }

  bool use_dictionary = (encoding == Encoding::kPlainDictionary ||
                         encoding == Encoding::kRleDictionary);

  if (use_dictionary) {
    return DecodeDictionaryValues<T>(data, num_values, def_levels, typed_col);
  }

  PlainDecoder<T> decoder;
  decoder.SetData(data);

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&decoder]() { return decoder.ReadValue(); },
      [typed_col](T val) { typed_col->Append(val); },
      [typed_col]() { typed_col->AppendNull(); });
}

template <typename T>
Status ColumnReader::DecodeDictionaryValues(
    std::span<const uint8_t> data, size_t num_values,
    const std::vector<int32_t>& def_levels, TypedColumnVector<T>* column) {
  PlainDecoder<T> dict_decoder;
  dict_decoder.SetData(dictionary_data_);
  std::vector<T> dictionary(dict_num_values_);
  if (dict_decoder.ReadBatch(dictionary) != dictionary.size()) {
    return Error::ColumnDecodeError("Dictionary page shorter than its value count");
  }

  // The data contains the bit width followed by RLE encoded indices.
  if (data.empty()) {
    return {};
  }
  int bit_width = data[0];
  auto indices_data = data.subspan(1);

  RleBitPackingDecoder index_decoder(bit_width);
  size_t non_null_count =
      def_levels.empty()
          ? num_values
          : std::count_if(def_levels.begin(), def_levels.end(),
                          [](int32_t level) { return level == 1; });
  index_decoder.SetData(indices_data, non_null_count);

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&index_decoder, &dictionary]() -> Result<T> {
        auto idx_result = index_decoder.ReadValue();
        if (!idx_result.has_value()) {
          return std::unexpected(idx_result.error());
        }
        int32_t idx = idx_result.value();
        if (idx < 0 || static_cast<size_t>(idx) >= dictionary.size()) {
          return std::unexpected(
              Error::OutOfRange("Dictionary index out of range"));
        }
        return dictionary[idx];
      },
      [column](T val) { column->Append(val); },
      [column]() { column->AppendNull(); });
}

Status ColumnReader::DecodeBinaryValues(Encoding encoding,
                                        std::span<const uint8_t> data,
                                        size_t num_values,
                                        const std::vector<int32_t>& def_levels,
                                        ColumnVector* column) {
  auto* str_col = dynamic_cast<StringColumnVector*>(column);
  if (!str_col) {
    return Error::TypeError("Column type mismatch for binary");
  }

  bool use_dictionary = (encoding == Encoding::kPlainDictionary ||
                         encoding == Encoding::kRleDictionary);

  if (use_dictionary) {
    return DecodeDictionaryBinaryValues(data, num_values, def_levels, str_col);
  }

  PlainBinaryDecoder decoder;
  decoder.SetData(data);

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&decoder]() { return decoder.ReadValue(); },
      [str_col](std::string val) { str_col->Append(std::move(val)); },
      [str_col]() { str_col->AppendNull(); });
}

Status ColumnReader::DecodeDictionaryBinaryValues(
    std::span<const uint8_t> data, size_t num_values,
    const std::vector<int32_t>& def_levels, StringColumnVector* column) {
  PlainBinaryDecoder dict_decoder;
  dict_decoder.SetData(dictionary_data_);
  std::vector<std::string> dictionary;
  dictionary.reserve(dict_num_values_);
  for (size_t i = 0; i < static_cast<size_t>(dict_num_values_); ++i) {
    auto result = dict_decoder.ReadValue();
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    dictionary.push_back(std::move(result.value()));
  }

  if (data.empty()) {
    return {};
  }
  int bit_width = data[0];
  auto indices_data = data.subspan(1);

  RleBitPackingDecoder index_decoder(bit_width);
  size_t non_null_count =
      def_levels.empty()
          ? num_values
          : std::count_if(def_levels.begin(), def_levels.end(),
                          [](int32_t level) { return level == 1; });
  index_decoder.SetData(indices_data, non_null_count);

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&index_decoder, &dictionary]() -> Result<std::string> {
        auto idx_result = index_decoder.ReadValue();
        if (!idx_result.has_value()) {
          return std::unexpected(idx_result.error());
        }
        int32_t idx = idx_result.value();
        if (idx < 0 || static_cast<size_t>(idx) >= dictionary.size()) {
          return std::unexpected(
              Error::OutOfRange("Dictionary index out of range"));
        }
        return dictionary[idx];
      },
      [column](std::string val) { column->Append(std::move(val)); },
      [column]() { column->AppendNull(); });
}

Status ColumnReader::DecodeFixedLenByteArrayValues(
    Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
    const std::vector<int32_t>& def_levels, ColumnVector* column) {
  auto* str_col = dynamic_cast<StringColumnVector*>(column);
  if (!str_col) {
    return Error::TypeError("Column type mismatch for fixed len byte array");
  }

  int32_t type_length = field_.type_length.value_or(0);
  size_t pos = 0;

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&data, &pos, type_length]() -> Result<std::string> {
        if (pos + type_length > data.size()) {
          return std::unexpected(Error::PageDecodeError(
              "Not enough data for fixed len byte array"));
        }
        std::string value(reinterpret_cast<const char*>(data.data() + pos),
                          type_length);
        pos += type_length;
        return value;
      },
      [str_col](std::string val) { str_col->Append(std::move(val)); },
      [str_col]() { str_col->AppendNull(); });
}

Status ColumnReader::DecodeInt96Values(
    Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
    const std::vector<int32_t>& def_levels, ColumnVector* column) {
  // INT96 is typically used for timestamps, treat as binary.
  auto* str_col = dynamic_cast<StringColumnVector*>(column);
  if (!str_col) {
    return Error::TypeError("Column type mismatch for INT96");
  }

  constexpr size_t INT96_SIZE = 12;
  size_t pos = 0;

  return ProcessWithDefLevels(
      num_values, def_levels,
      [&data, &pos]() -> Result<std::string> {
        if (pos + INT96_SIZE > data.size()) {
          return std::unexpected(
              Error::PageDecodeError("Not enough data for INT96"));
        }
        std::string value(reinterpret_cast<const char*>(data.data() + pos),
                          INT96_SIZE);
        pos += INT96_SIZE;
        return value;
      },
      [str_col](std::string val) { str_col->Append(std::move(val)); },
      [str_col]() { str_col->AppendNull(); });
}

}  // namespace quest::io::parquet
