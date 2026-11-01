// Copyright 2024
// Licensed under the Apache License, Version 2.0

#pragma once

#include <memory>
#include <span>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "quest/io/parquet/format.hpp"
#include "quest/common/result.hpp"
#include "quest/io/parquet/compression.hpp"
#include "quest/io/parquet/metadata.hpp"
#include "quest/io/parquet/page_reader.hpp"

namespace quest::io::parquet {

using columnar::ColumnBuilder;
using columnar::ColumnVector;
using columnar::ColumnVectorPtr;
using columnar::Field;

class ParquetFileReader;

// =============================================================================
// Column Reader
// =============================================================================

/// Decodes pages from a column chunk: Plain, Dictionary, and RLE encodings,
/// with nullable-column support via definition levels.
class ColumnReader {
 public:
  ColumnReader(const Field& field, const LeafType& leaf, const ColumnChunkMetadata& column_meta,
               std::span<const uint8_t> data,
               std::unique_ptr<CompressionCodec> codec)
      : field_(field),
        leaf_(leaf),
        column_meta_(column_meta),
        page_reader_(data),
        codec_(std::move(codec)) {}

  [[nodiscard]] Result<ColumnVectorPtr> ReadAll();

 private:
  /// @brief @p physical (decoded as the physical type) as the leaf's DataType.
  [[nodiscard]] Result<ColumnVectorPtr> Convert(const ColumnVector& physical) const;

  // ===========================================================================
  // Decompression
  // ===========================================================================

  [[nodiscard]] Result<std::span<const uint8_t>> DecompressPageData(
      const PageHeader& header, std::span<const uint8_t> compressed_data);

  // ===========================================================================
  // Page Reading
  // ===========================================================================

  void ReadDictionaryPage(const PageHeader& header,
                          std::span<const uint8_t> data);

  /// @brief Decodes every page of the chunk into @p builder.
  template <typename T>
  [[nodiscard]] Status ReadPages(ColumnBuilder<T>& builder);

  template <typename T>
  [[nodiscard]] Status ReadDataPage(const PageHeader& header, std::span<const uint8_t> data,
                                    ColumnBuilder<T>& builder);

  template <typename T>
  [[nodiscard]] Status ReadDataPageV2(const PageHeader& header, std::span<const uint8_t> data,
                                      ColumnBuilder<T>& builder);

  // ===========================================================================
  // Definition Level Decoding
  // ===========================================================================

  [[nodiscard]] Result<std::vector<int32_t>> DecodeDefinitionLevelsRle(
      std::span<const uint8_t> data, int max_level, size_t num_values);

  // ===========================================================================
  // Value Decoding Dispatch
  // ===========================================================================

  /// @brief Decodes one page's values; @p T (the builder's CType) picks the decoder.
  template <typename T>
  [[nodiscard]] Status DecodeValues(Encoding encoding, std::span<const uint8_t> data,
                                    size_t num_values, const std::vector<int32_t>& def_levels,
                                    ColumnBuilder<T>& builder);

  // ===========================================================================
  // Type-Specific Decoders
  // ===========================================================================

  [[nodiscard]] Status DecodeBooleanValues(std::span<const uint8_t> data, size_t num_values,
                                           const std::vector<int32_t>& def_levels,
                                           ColumnBuilder<bool>& builder);

  template <typename T>
  [[nodiscard]] Status DecodeFixedWidthValues(Encoding encoding, std::span<const uint8_t> data,
                                              size_t num_values,
                                              const std::vector<int32_t>& def_levels,
                                              ColumnBuilder<T>& builder);

  [[nodiscard]] Status DecodeBinaryValues(Encoding encoding, std::span<const uint8_t> data,
                                          size_t num_values,
                                          const std::vector<int32_t>& def_levels,
                                          ColumnBuilder<std::string_view>& builder);

  [[nodiscard]] Status DecodeInt96Values(Encoding encoding, std::span<const uint8_t> data,
                                         size_t num_values,
                                         const std::vector<int32_t>& def_levels,
                                         ColumnBuilder<columnar::Timestamp>& builder);

  Field field_;
  LeafType leaf_;
  ColumnChunkMetadata column_meta_;
  PageReader page_reader_;
  std::unique_ptr<CompressionCodec> codec_;

  bool has_dictionary_ = false;
  std::vector<uint8_t> dictionary_data_;
  int32_t dict_num_values_ = 0;

  // Decompression buffer (reused across pages).
  std::vector<uint8_t> decompression_buffer_;
};

}  // namespace quest::io::parquet
