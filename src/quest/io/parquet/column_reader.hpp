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

using columnar::ColumnVector;
using columnar::CreateColumnVector;
using columnar::Field;
using columnar::StringColumnVector;
using columnar::TypedColumnVector;

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
        data_(data),
        page_reader_(data),
        codec_(std::move(codec)) {}

  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> ReadAll();

 private:
  /// @brief @p physical (decoded as the physical type) as the leaf's DataType.
  [[nodiscard]] Result<std::shared_ptr<ColumnVector>> Convert(const ColumnVector& physical) const;

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

  [[nodiscard]] Status ReadDataPage(const PageHeader& header,
                                     std::span<const uint8_t> data,
                                     ColumnVector* column);

  [[nodiscard]] Status ReadDataPageV2(const PageHeader& header,
                                       std::span<const uint8_t> data,
                                       ColumnVector* column);

  // ===========================================================================
  // Definition Level Decoding
  // ===========================================================================

  [[nodiscard]] Result<std::vector<int32_t>> DecodeDefinitionLevelsRle(
      std::span<const uint8_t> data, int max_level, size_t num_values);

  // ===========================================================================
  // Value Decoding Dispatch
  // ===========================================================================

  [[nodiscard]] Status DecodeValues(Encoding encoding,
                                     std::span<const uint8_t> data,
                                     size_t num_values,
                                     const std::vector<int32_t>& def_levels,
                                     ColumnVector* column);

  // ===========================================================================
  // Type-Specific Decoders
  // ===========================================================================

  [[nodiscard]] Status DecodeBooleanValues(
      Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column);

  template <typename T>
  [[nodiscard]] Status DecodeFixedWidthValues(
      Encoding encoding, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column);

  template <typename T>
  [[nodiscard]] Status DecodeDictionaryValues(
      std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, TypedColumnVector<T>* column);

  [[nodiscard]] Status DecodeBinaryValues(Encoding encoding,
                                           std::span<const uint8_t> data,
                                           size_t num_values,
                                           const std::vector<int32_t>& def_levels,
                                           ColumnVector* column);

  [[nodiscard]] Status DecodeDictionaryBinaryValues(
      std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, StringColumnVector* column);

  [[nodiscard]] Status DecodeInt96Values(
      Encoding /*encoding*/, std::span<const uint8_t> data, size_t num_values,
      const std::vector<int32_t>& def_levels, ColumnVector* column);

  Field field_;
  LeafType leaf_;
  ColumnChunkMetadata column_meta_;
  std::span<const uint8_t> data_;
  PageReader page_reader_;
  std::unique_ptr<CompressionCodec> codec_;

  bool has_dictionary_ = false;
  std::vector<uint8_t> dictionary_data_;
  int32_t dict_num_values_ = 0;

  // Decompression buffer (reused across pages).
  std::vector<uint8_t> decompression_buffer_;
};

}  // namespace quest::io::parquet
