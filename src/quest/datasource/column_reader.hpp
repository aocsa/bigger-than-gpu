// Copyright 2024
// Licensed under the Apache License, Version 2.0

#ifndef PARQUET_READER_COLUMN_READER_HPP
#define PARQUET_READER_COLUMN_READER_HPP

/// @file column_reader.hpp
/// @brief Column reader for decoding Parquet column chunks.
///
/// This file provides the ColumnReader class which handles reading and
/// decoding values from Parquet column chunks, supporting various encodings
/// and data types.

#include <algorithm>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

#include "column_vector.hpp"
#include "encoding.hpp"
#include "metadata.hpp"
#include "page_reader.hpp"
#include "quest/result.hpp"
#include "schema.hpp"
#include "types.hpp"

namespace quest::datasource {

// Forward declaration
class ParquetFileReader;

// =============================================================================
// Column Reader
// =============================================================================

/// @brief Column reader that decodes values from a Parquet column chunk.
///
/// This class handles reading pages from a column chunk and decoding values
/// according to their encoding (Plain, Dictionary, RLE). It supports nullable
/// columns with definition levels.
class ColumnReader {
 public:
  /// @brief Constructs a column reader.
  /// @param field The schema field for this column.
  /// @param column_meta The column chunk metadata.
  /// @param data The raw bytes of the column chunk.
  ColumnReader(const Field& field, const ColumnChunkMetadata& column_meta,
               std::span<const uint8_t> data)
      : field_(field),
        column_meta_(column_meta),
        data_(data),
        page_reader_(data, column_meta.type,
                     field.IsNullable() ? 1 : 0,  // max def level
                     0)                           // max rep level for flat schema
  {}

  /// @brief Reads all values from this column chunk into a column vector.
  /// @return The column vector on success, or decode error.
  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> ReadAll() {
    size_t total_values = column_meta_.num_values;
    auto column = CreateColumnVector(field_.data_type, total_values);

    while (page_reader_.HasNextPage()) {
      auto header_result = page_reader_.ReadPageHeader();
      if (!header_result.has_value()) {
        return std::unexpected(header_result.error());
      }
      auto header = header_result.value();

      auto page_data_result = page_reader_.GetPageData();
      if (!page_data_result.has_value()) {
        return std::unexpected(page_data_result.error());
      }
      auto page_data = page_data_result.value();

      switch (header.type) {
        case PageType::kDictionaryPage:
          ReadDictionaryPage(header, page_data);
          break;
        case PageType::kDataPage: {
          auto status = ReadDataPage(header, page_data, column.get());
          if (!status.ok()) {
            return std::unexpected(status);
          }
          break;
        }
        case PageType::kDataPageV2: {
          auto status = ReadDataPageV2(header, page_data, column.get());
          if (!status.ok()) {
            return std::unexpected(status);
          }
          break;
        }
        default:
          // Skip unknown page types.
          break;
      }
    }

    return column;
  }

 private:
  // ===========================================================================
  // Page Reading
  // ===========================================================================

  void ReadDictionaryPage(const PageHeader& header,
                          std::span<const uint8_t> data) {
    // Store dictionary for later use with dictionary-encoded pages.
    dictionary_data_ = std::vector<uint8_t>(data.begin(), data.end());
    dict_num_values_ = header.num_dict_values;
    has_dictionary_ = true;
  }

  [[nodiscard]] Status ReadDataPage(const PageHeader& header,
                                     std::span<const uint8_t> data,
                                     ColumnVector* column) {
    size_t pos = 0;
    int max_def_level = field_.IsNullable() ? 1 : 0;

    // Read definition levels if column is nullable.
    std::vector<int32_t> def_levels;
    if (max_def_level > 0) {
      // Definition levels are prefixed with their byte length.
      if (pos + 4 > data.size()) {
        return Status::PageDecodeError(
            "Not enough data for definition level length");
      }
      uint32_t def_level_length;
      std::memcpy(&def_level_length, data.data() + pos, 4);
      pos += 4;

      auto levels_result = DecodeDefinitionLevelsRle(
          data.subspan(pos, def_level_length), max_def_level, header.num_values);
      if (!levels_result.has_value()) {
        return levels_result.error();
      }
      def_levels = std::move(levels_result.value());
      pos += def_level_length;
    }

    // Remaining data contains the values.
    auto values_data = data.subspan(pos);

    // Decode values based on encoding.
    return DecodeValues(header.encoding, values_data, header.num_values,
                        def_levels, column);
  }

  [[nodiscard]] Status ReadDataPageV2(const PageHeader& header,
                                       std::span<const uint8_t> data,
                                       ColumnVector* column) {
    size_t pos = 0;

    // Skip repetition levels (always 0 for flat schema).
    pos += header.repetition_levels_byte_length;

    // Read definition levels.
    std::vector<int32_t> def_levels;
    int max_def_level = field_.IsNullable() ? 1 : 0;
    if (max_def_level > 0 && header.definition_levels_byte_length > 0) {
      auto def_data = data.subspan(pos, header.definition_levels_byte_length);
      auto levels_result =
          DecodeDefinitionLevelsRle(def_data, max_def_level, header.num_values);
      if (!levels_result.has_value()) {
        return levels_result.error();
      }
      def_levels = std::move(levels_result.value());
      pos += header.definition_levels_byte_length;
    }

    // Remaining data contains the values.
    auto values_data = data.subspan(pos);

    // V2 pages store non-null value count.
    size_t num_non_null = header.num_values - header.num_nulls;
    return DecodeValues(header.encoding, values_data, num_non_null, def_levels,
                        column);
  }

  // ===========================================================================
  // Definition Level Decoding
  // ===========================================================================

  [[nodiscard]] Result<std::vector<int32_t>> DecodeDefinitionLevelsRle(
      std::span<const uint8_t> data, int max_level, size_t num_values) {
    std::vector<int32_t> levels(num_values);
    int bit_width = ComputeBitWidth(max_level);
    RleBitPackingDecoder decoder(bit_width);
    decoder.SetData(data, num_values);
    auto status = decoder.ReadBatch(levels);
    if (!status.ok()) {
      return std::unexpected(status);
    }
    return levels;
  }

  // ===========================================================================
  // Value Decoding Dispatch
  // ===========================================================================

  [[nodiscard]] Status DecodeValues(Encoding encoding,
                                     std::span<const uint8_t> data,
                                     size_t num_values,
                                     const std::vector<int32_t>& def_levels,
                                     ColumnVector* column) {
    bool use_dictionary = (encoding == Encoding::kPlainDictionary ||
                           encoding == Encoding::kRleDictionary);

    if (use_dictionary && !has_dictionary_) {
      return Status::ColumnDecodeError(
          "Dictionary encoding but no dictionary page found");
    }

    // Dispatch based on physical type.
    switch (column_meta_.type) {
      case PhysicalType::kBoolean:
        return DecodeBooleanValues(encoding, data, num_values, def_levels,
                                   column);
      case PhysicalType::kInt32:
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
    return Status::OK();
  }

  // ===========================================================================
  // Type-Specific Decoders
  // ===========================================================================

  [[nodiscard]] Status DecodeBooleanValues(
      Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column) {
    auto* typed_col = dynamic_cast<TypedColumnVector<bool>*>(column);
    if (!typed_col) {
      return Status::TypeError("Column type mismatch for boolean");
    }

    PlainDecoder<bool> decoder;
    decoder.SetData(data);
    decoder.SetNumValues(num_values);

    if (def_levels.empty()) {
      // All values present.
      for (size_t i = 0; i < num_values; ++i) {
        auto result = decoder.ReadValue();
        if (!result.has_value()) {
          return result.error();
        }
        typed_col->Append(result.value());
      }
    } else {
      // Check definition levels for nulls.
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          auto result = decoder.ReadValue();
          if (!result.has_value()) {
            return result.error();
          }
          typed_col->Append(result.value());
        } else {
          typed_col->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  template <typename T>
  [[nodiscard]] Status DecodeFixedWidthValues(
      Encoding encoding, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column) {
    auto* typed_col = dynamic_cast<TypedColumnVector<T>*>(column);
    if (!typed_col) {
      return Status::TypeError("Column type mismatch");
    }

    bool use_dictionary = (encoding == Encoding::kPlainDictionary ||
                           encoding == Encoding::kRleDictionary);

    if (use_dictionary) {
      return DecodeDictionaryValues<T>(data, num_values, def_levels, typed_col);
    }

    PlainDecoder<T> decoder;
    decoder.SetData(data);

    if (def_levels.empty()) {
      for (size_t i = 0; i < num_values; ++i) {
        auto result = decoder.ReadValue();
        if (!result.has_value()) {
          return result.error();
        }
        typed_col->Append(result.value());
      }
    } else {
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          auto result = decoder.ReadValue();
          if (!result.has_value()) {
            return result.error();
          }
          typed_col->Append(result.value());
        } else {
          typed_col->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  template <typename T>
  [[nodiscard]] Status DecodeDictionaryValues(
      std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, TypedColumnVector<T>* column) {
    // First, decode the dictionary.
    PlainDecoder<T> dict_decoder;
    dict_decoder.SetData(dictionary_data_);
    std::vector<T> dictionary(dict_num_values_);
    dict_decoder.ReadBatch(dictionary);

    // The data contains the bit width followed by RLE encoded indices.
    if (data.empty()) {
      return Status::OK();
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

    if (def_levels.empty()) {
      for (size_t i = 0; i < num_values; ++i) {
        auto idx_result = index_decoder.ReadValue();
        if (!idx_result.has_value()) {
          return idx_result.error();
        }
        int32_t idx = idx_result.value();
        if (idx < 0 || static_cast<size_t>(idx) >= dictionary.size()) {
          return Status::OutOfRange("Dictionary index out of range");
        }
        column->Append(dictionary[idx]);
      }
    } else {
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          auto idx_result = index_decoder.ReadValue();
          if (!idx_result.has_value()) {
            return idx_result.error();
          }
          int32_t idx = idx_result.value();
          if (idx < 0 || static_cast<size_t>(idx) >= dictionary.size()) {
            return Status::OutOfRange("Dictionary index out of range");
          }
          column->Append(dictionary[idx]);
        } else {
          column->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  [[nodiscard]] Status DecodeBinaryValues(Encoding encoding,
                                           std::span<const uint8_t> data,
                                           size_t num_values,
                                           const std::vector<int32_t>& def_levels,
                                           ColumnVector* column) {
    auto* str_col = dynamic_cast<StringColumnVector*>(column);
    if (!str_col) {
      return Status::TypeError("Column type mismatch for binary");
    }

    bool use_dictionary = (encoding == Encoding::kPlainDictionary ||
                           encoding == Encoding::kRleDictionary);

    if (use_dictionary) {
      return DecodeDictionaryBinaryValues(data, num_values, def_levels, str_col);
    }

    PlainBinaryDecoder decoder;
    decoder.SetData(data);

    if (def_levels.empty()) {
      for (size_t i = 0; i < num_values; ++i) {
        auto result = decoder.ReadValue();
        if (!result.has_value()) {
          return result.error();
        }
        str_col->Append(std::move(result.value()));
      }
    } else {
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          auto result = decoder.ReadValue();
          if (!result.has_value()) {
            return result.error();
          }
          str_col->Append(std::move(result.value()));
        } else {
          str_col->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  [[nodiscard]] Status DecodeDictionaryBinaryValues(
      std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, StringColumnVector* column) {
    // Decode dictionary.
    PlainBinaryDecoder dict_decoder;
    dict_decoder.SetData(dictionary_data_);
    std::vector<std::string> dictionary;
    dictionary.reserve(dict_num_values_);
    for (size_t i = 0; i < static_cast<size_t>(dict_num_values_); ++i) {
      auto result = dict_decoder.ReadValue();
      if (!result.has_value()) {
        return result.error();
      }
      dictionary.push_back(std::move(result.value()));
    }

    // Decode indices.
    if (data.empty()) {
      return Status::OK();
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

    if (def_levels.empty()) {
      for (size_t i = 0; i < num_values; ++i) {
        auto idx_result = index_decoder.ReadValue();
        if (!idx_result.has_value()) {
          return idx_result.error();
        }
        int32_t idx = idx_result.value();
        if (idx < 0 || static_cast<size_t>(idx) >= dictionary.size()) {
          return Status::OutOfRange("Dictionary index out of range");
        }
        column->Append(dictionary[idx]);
      }
    } else {
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          auto idx_result = index_decoder.ReadValue();
          if (!idx_result.has_value()) {
            return idx_result.error();
          }
          int32_t idx = idx_result.value();
          if (idx < 0 || static_cast<size_t>(idx) >= dictionary.size()) {
            return Status::OutOfRange("Dictionary index out of range");
          }
          column->Append(dictionary[idx]);
        } else {
          column->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  [[nodiscard]] Status DecodeFixedLenByteArrayValues(
      Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column) {
    auto* str_col = dynamic_cast<StringColumnVector*>(column);
    if (!str_col) {
      return Status::TypeError("Column type mismatch for fixed len byte array");
    }

    int32_t type_length = field_.type_length.value_or(0);
    size_t pos = 0;

    if (def_levels.empty()) {
      for (size_t i = 0; i < num_values; ++i) {
        if (pos + type_length > data.size()) {
          return Status::PageDecodeError(
              "Not enough data for fixed len byte array");
        }
        std::string value(reinterpret_cast<const char*>(data.data() + pos),
                          type_length);
        str_col->Append(std::move(value));
        pos += type_length;
      }
    } else {
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          if (pos + type_length > data.size()) {
            return Status::PageDecodeError(
                "Not enough data for fixed len byte array");
          }
          std::string value(reinterpret_cast<const char*>(data.data() + pos),
                            type_length);
          str_col->Append(std::move(value));
          pos += type_length;
        } else {
          str_col->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  [[nodiscard]] Status DecodeInt96Values(
      Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column) {
    // INT96 is typically used for timestamps, treat as binary.
    auto* str_col = dynamic_cast<StringColumnVector*>(column);
    if (!str_col) {
      return Status::TypeError("Column type mismatch for INT96");
    }

    constexpr size_t INT96_SIZE = 12;
    size_t pos = 0;

    if (def_levels.empty()) {
      for (size_t i = 0; i < num_values; ++i) {
        if (pos + INT96_SIZE > data.size()) {
          return Status::PageDecodeError("Not enough data for INT96");
        }
        std::string value(reinterpret_cast<const char*>(data.data() + pos),
                          INT96_SIZE);
        str_col->Append(std::move(value));
        pos += INT96_SIZE;
      }
    } else {
      for (size_t i = 0; i < def_levels.size(); ++i) {
        if (def_levels[i] == 1) {
          if (pos + INT96_SIZE > data.size()) {
            return Status::PageDecodeError("Not enough data for INT96");
          }
          std::string value(reinterpret_cast<const char*>(data.data() + pos),
                            INT96_SIZE);
          str_col->Append(std::move(value));
          pos += INT96_SIZE;
        } else {
          str_col->AppendNull();
        }
      }
    }
    return Status::OK();
  }

  // ===========================================================================
  // Member Variables
  // ===========================================================================

  Field field_;
  ColumnChunkMetadata column_meta_;
  std::span<const uint8_t> data_;
  PageReader page_reader_;

  // Dictionary state.
  bool has_dictionary_ = false;
  std::vector<uint8_t> dictionary_data_;
  int32_t dict_num_values_ = 0;
};

}  // namespace quest::datasource

#endif  // PARQUET_READER_COLUMN_READER_HPP
