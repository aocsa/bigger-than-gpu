// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_parquet_reader.cpp
/// @brief Unit tests for Parquet file reading functionality.

#include <gtest/gtest.h>

#include <climits>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "quest/io/parquet/bytes.hpp"
#include "quest/io/parquet/encoding.hpp"
#include "quest/io/parquet/file_reader.hpp"
#include "quest/io/parquet/page_reader.hpp"
#include "quest/io/parquet/thrift_decoder.hpp"
#include "quest/io/parquet/parquet_data_source.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::columnar;
using namespace quest::io::parquet;

// =============================================================================
// Test Fixture
// =============================================================================

class ParquetReaderTest : public ::testing::Test {
 protected:
  std::filesystem::path test_data_dir_ =
      std::filesystem::path(__FILE__).parent_path().parent_path() / "examples";

  std::filesystem::path employees_file() const {
    return test_data_dir_ / "employees_uncompressed.parquet";
  }

  std::filesystem::path string_test_file() const {
    return test_data_dir_ / "string_test.parquet";
  }
};

// =============================================================================
// ParquetDataSource Tests
// =============================================================================

TEST_F(ParquetReaderTest, OpenValidFile) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  EXPECT_GT(ds.schema().num_fields(), 0U);
}

TEST_F(ParquetReaderTest, OpenInvalidFile) {
  auto result = ParquetDataSource::Open("/nonexistent/file.parquet");
  EXPECT_FALSE(result.has_value());
}

TEST_F(ParquetReaderTest, SchemaFields) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  EXPECT_GT(schema.num_fields(), 0U);
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    EXPECT_FALSE(schema.field(i).name.empty());
  }
}

TEST_F(ParquetReaderTest, NumRows) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  EXPECT_GT(ds.num_rows(), 0);
}

TEST_F(ParquetReaderTest, ScanAllColumns) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  std::vector<std::string> all_columns;
  for (size_t i = 0; i < schema.num_fields(); ++i) {
    all_columns.push_back(schema.field(i).name);
  }

  size_t batch_count = 0;
  size_t total_rows = 0;

  for (size_t i = 0; i < ds.num_partitions(); ++i) {
    auto batch_result = ds.ReadPartition(i, all_columns);
    ASSERT_TRUE(batch_result.has_value())
        << "Batch read failed: " << batch_result.error().ToString();
    total_rows += batch_result.value().num_rows();
    ++batch_count;
  }

  EXPECT_GT(batch_count, 0U);
  EXPECT_EQ(total_rows, static_cast<size_t>(ds.num_rows()));
}

TEST_F(ParquetReaderTest, ScanProjection) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  if (schema.num_fields() < 2) {
    GTEST_SKIP() << "Need at least 2 columns for projection test";
  }

  std::vector<std::string> projection = {schema.field(0).name};

  for (size_t i = 0; i < ds.num_partitions(); ++i) {
    auto batch_result = ds.ReadPartition(i, projection);
    ASSERT_TRUE(batch_result.has_value());
    EXPECT_EQ(batch_result.value().num_columns(), 1U);
    EXPECT_EQ(batch_result.value().schema().field(0).name, schema.field(0).name);
  }
}

TEST_F(ParquetReaderTest, Utf8ColumnReadsAsString) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  // employees.parquet marks `name` as BYTE_ARRAY with the UTF8 / STRING annotation.
  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto index = ds.schema().GetFieldIndex("name");
  ASSERT_TRUE(index.has_value());
  EXPECT_EQ(ds.schema().field(*index).data_type, DataType::kString);

  ASSERT_OK_AND_ASSIGN(auto batch, ds.ReadPartition(0, {"name"}));
  EXPECT_EQ(batch.column(0).GetType(), DataType::kString);
  EXPECT_FALSE(batch.column(0).IsNull(0));
}

TEST_F(ParquetReaderTest, NumericColumnTypes) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));
  auto schema = ds.schema();

  for (size_t i = 0; i < schema.num_fields(); ++i) {
    const auto& field = schema.field(i);
    bool is_numeric = (field.data_type == DataType::kInt32) ||
                      (field.data_type == DataType::kInt64) ||
                      (field.data_type == DataType::kFloat) ||
                      (field.data_type == DataType::kDouble);

    if (is_numeric) {
      for (size_t i = 0; i < ds.num_partitions(); ++i) {
        auto batch_result = ds.ReadPartition(i, {field.name});
        ASSERT_TRUE(batch_result.has_value());
        EXPECT_EQ(batch_result.value().num_columns(), 1U);
      }
      break;  // Test just one numeric column
    }
  }
}

TEST_F(ParquetReaderTest, MetadataAccess) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));

  EXPECT_GT(ds.num_row_groups(), 0U);
  EXPECT_GT(ds.num_rows(), 0);

  const auto& metadata = ds.metadata();
  EXPECT_GT(metadata.num_rows, 0);
}

// RowGroup thrift ids: 4 sorting_columns, 5 file_offset, 6 total_compressed_size, 7 ordinal.
TEST(ParquetMetadataTest, RowGroupOffsetAndSizeMatchDuckDb) {
  // Values from DuckDB's parquet_metadata(); 43246 does not fit an i16.
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" /
                    "employees_uncompressed.parquet";
  ASSERT_OK_AND_ASSIGN(auto reader, ParquetFileReader::Open(path.string()));
  ASSERT_GE(reader.metadata().row_groups.size(), 2U);
  const RowGroupMetadata& rg = reader.metadata().row_groups[1];
  EXPECT_EQ(rg.file_offset, 43314);
  EXPECT_EQ(rg.total_compressed_size, 43246);
}

TEST(ParquetMetadataTest, RowGroupSortingColumnsAreSkippedAndOrdinalIsRead) {
  // No file under testdata/ writes sorting_columns or ordinal, so the footer is hand-encoded.
  const std::vector<uint8_t> footer{
      0x49, 0x1C,        // FileMetadata field 4 row_groups: list of 1 struct
      0x49, 0x1C,        //   RowGroup field 4 sorting_columns: list of 1 struct
      0x15, 0x06, 0x00,  //     SortingColumn field 1 column_idx = 3, stop
      0x16, 0x08,        //   field 5 file_offset = 4
      0x16, 0xD8, 0x04,  //   field 6 total_compressed_size = 300
      0x14, 0x04,        //   field 7 ordinal = 2
      0x00,              //   RowGroup stop
      0x00};             // FileMetadata stop
  ASSERT_OK_AND_ASSIGN(auto metadata, MetadataParser::Parse(footer));
  ASSERT_EQ(metadata.row_groups.size(), 1U);
  const RowGroupMetadata& rg = metadata.row_groups[0];
  EXPECT_EQ(rg.file_offset, 4);
  EXPECT_EQ(rg.total_compressed_size, 300);
  EXPECT_EQ(rg.ordinal, 2);
}

TEST_F(ParquetReaderTest, EmptyProjection) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));

  // Empty projection should still work (may scan all or none depending on impl)
  std::vector<std::string> empty_projection;

  // Just verify it doesn't crash
  for (size_t i = 0; i < ds.num_partitions(); ++i) {
    (void)ds.ReadPartition(i, empty_projection);
  }
}

TEST_F(ParquetReaderTest, InvalidColumnProjection) {
  if (!std::filesystem::exists(employees_file())) {
    GTEST_SKIP() << "Test file not found: " << employees_file();
  }

  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(employees_file().string()));

  std::vector<std::string> bad_projection = {"nonexistent_column_xyz"};

  // The scan should either fail or return empty batches
  for (size_t i = 0; i < ds.num_partitions(); ++i) {
    auto batch_result = ds.ReadPartition(i, bad_projection);
    // Either fails or returns batch with 0 columns
    if (batch_result.has_value()) {
      EXPECT_EQ(batch_result.value().num_columns(), 0U);
    }
  }
}

// =============================================================================
// Corrupt input: every length comes from the file and must be bounds-checked
// =============================================================================

TEST(ParquetBytesTest, SliceAndLoadRejectOutOfBounds) {
  const std::vector<uint8_t> buf{0x01, 0x02, 0x03, 0x04, 0x05};
  ASSERT_OK_AND_ASSIGN(auto value, LoadLE<uint32_t>(buf, 1));
  EXPECT_EQ(value, 0x05040302U);
  EXPECT_FALSE(LoadLE<uint32_t>(buf, 2).has_value());
  EXPECT_FALSE(Slice(buf, 3, 3).has_value());
  EXPECT_FALSE(Slice(buf, 6, 0).has_value());
  EXPECT_FALSE(Slice(buf, 1, SIZE_MAX).has_value());
}

TEST(ParquetBytesTest, BinaryDecoderRejectsLengthPastBuffer) {
  const std::vector<uint8_t> buf{0xFF, 0xFF, 0xFF, 0x7F, 'a', 'b'};
  PlainBinaryDecoder decoder;
  decoder.SetData(buf);
  EXPECT_FALSE(decoder.ReadValue().has_value());
}

TEST(ParquetBytesTest, RleDecoderRejectsTruncatedBitPackedRun) {
  // Header 0x03: one bit-packed group of 8 values at bit width 8 needs 8 bytes; 2 are present.
  const std::vector<uint8_t> buf{0x03, 0x01, 0x02};
  RleBitPackingDecoder decoder(8);
  decoder.SetData(buf);
  ASSERT_OK_AND_ASSIGN(auto first, decoder.ReadValue());
  EXPECT_EQ(first, 1);
  ASSERT_OK_AND_ASSIGN(auto second, decoder.ReadValue());
  EXPECT_EQ(second, 2);
  EXPECT_FALSE(decoder.ReadValue().has_value());
}

TEST(ParquetBytesTest, RleDecoderRejectsInvalidBitWidth) {
  const std::vector<uint8_t> buf{0x02, 0x01};
  RleBitPackingDecoder decoder(40);
  decoder.SetData(buf);
  EXPECT_FALSE(decoder.ReadValue().has_value());
}

TEST(ParquetCorruptFileTest, FooterLengthPastFileStartIsAnError) {
  const auto path = std::filesystem::temp_directory_path() / "quest_corrupt_footer.parquet";
  {
    std::ofstream out(path, std::ios::binary);
    const char bytes[] = {'P', 'A', 'R', '1', 0, 0, 0, 0, '\xFF', '\xFF', '\xFF', '\x7F',
                          'P', 'A', 'R', '1'};
    out.write(bytes, sizeof(bytes));
  }
  auto result = ParquetDataSource::Open(path.string());
  EXPECT_FALSE(result.has_value());
  std::filesystem::remove(path);
}

TEST(ParquetCorruptFileTest, RowGroupWithTooFewColumnChunksIsAnError) {
  // Compact-thrift FileMetadata: version 1; schema = root + one required INT32 leaf "a";
  // num_rows 0; one row group with an empty columns list.
  const std::vector<uint8_t> footer{
      0x15, 0x02,                                      // 1: version = 1
      0x19, 0x2C,                                      // 2: schema, list of 2 structs
      0x48, 0x04, 'r', 'o', 'o', 't', 0x15, 0x02, 0x00,  //   root: name, num_children = 1
      0x15, 0x02, 0x25, 0x00, 0x18, 0x01, 'a', 0x00,   //   leaf: INT32, REQUIRED, name "a"
      0x16, 0x00,                                      // 3: num_rows = 0
      0x19, 0x1C,                                      // 4: row_groups, list of 1 struct
      0x19, 0x0C, 0x16, 0x00, 0x16, 0x00, 0x00,        //   columns = [], sizes 0
      0x00};
  const auto path = std::filesystem::temp_directory_path() / "quest_missing_chunks.parquet";
  {
    std::ofstream out(path, std::ios::binary);
    const auto length = static_cast<uint32_t>(footer.size());
    out.write("PAR1", 4);
    out.write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(length));
    out.write(reinterpret_cast<const char*>(&length), sizeof(length));
    out.write("PAR1", 4);
  }
  ASSERT_OK_AND_ASSIGN(auto reader, ParquetFileReader::Open(path.string()));
  auto batch = reader.ReadRowGroup(0, {});
  ASSERT_FALSE(batch.has_value());
  EXPECT_EQ(batch.error().code(), quest::ErrorCode::ParquetFormatError);
  std::filesystem::remove(path);
}

TEST(ParquetCorruptFileTest, NegativeCompressedPageSizeIsAnError) {
  // Compact-thrift PageHeader: type=DATA_PAGE, uncompressed_page_size=10,
  // compressed_page_size=-1 (zigzag 0x01), stop.
  const std::vector<uint8_t> page{0x15, 0x00, 0x15, 0x14, 0x15, 0x01, 0x00, 0xAA, 0xBB};
  PageReader reader(page);
  ASSERT_OK(reader.ReadPageHeader());
  EXPECT_FALSE(reader.GetPageData().has_value());
}

TEST(ParquetPageHeaderTest, DataPageV2HeaderIsParsed) {
  // Compact-thrift PageHeader: type=DATA_PAGE_V2, uncompressed and compressed size 4,
  // 8: data_page_header_v2 {num_values=5, num_nulls=1, num_rows=4, encoding=RLE_DICTIONARY,
  // definition_levels_byte_length=2, repetition_levels_byte_length=0, is_compressed=false}.
  const std::vector<uint8_t> page{
      0x15, 0x06, 0x15, 0x08, 0x15, 0x08,  // 1: type = 3, 2: uncompressed = 4, 3: compressed = 4
      0x5C,                                // 8: data_page_header_v2 (struct)
      0x15, 0x0A, 0x15, 0x02, 0x15, 0x08,  //   num_values = 5, num_nulls = 1, num_rows = 4
      0x15, 0x10, 0x15, 0x04, 0x15, 0x00,  //   encoding = 8, def length = 2, rep length = 0
      0x12, 0x00,                          //   7: is_compressed = false (bool in header), stop
      0x00,                                // stop
      0xA1, 0xA2, 0xA3, 0xA4};
  PageReader reader(page);
  ASSERT_OK_AND_ASSIGN(auto header, reader.ReadPageHeader());
  EXPECT_EQ(header.type, PageType::kDataPageV2);
  EXPECT_EQ(header.uncompressed_page_size, 4);
  EXPECT_EQ(header.compressed_page_size, 4);
  EXPECT_EQ(header.num_values, 5);
  EXPECT_EQ(header.num_nulls, 1);
  EXPECT_EQ(header.num_rows, 4);
  EXPECT_EQ(header.encoding, Encoding::kRleDictionary);
  EXPECT_EQ(header.definition_levels_byte_length, 2);
  EXPECT_EQ(header.repetition_levels_byte_length, 0);
  EXPECT_FALSE(header.is_compressed);
  ASSERT_OK_AND_ASSIGN(auto data, reader.GetPageData());
  ASSERT_EQ(data.size(), 4U);
  EXPECT_EQ(data[0], 0xA1);
  EXPECT_FALSE(reader.HasNextPage());
}

TEST(ParquetCorruptFileTest, ListSizeBeyondBufferIsAnError) {
  // List header: size nibble 0xF -> varint size 0x7FFFFFFF, element type i32; no elements follow.
  const std::vector<uint8_t> buf{0xF5, 0xFF, 0xFF, 0xFF, 0xFF, 0x07};
  ThriftDecoder decoder(buf);
  EXPECT_FALSE(decoder.ReadListHeader().has_value());
}

TEST(ParquetCorruptFileTest, UnsignedVarintLongerThanFiveBytesIsAnError) {
  // Six continuation bytes: a uint32 varint has 5 bytes at most.
  const std::vector<uint8_t> buf{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
  ThriftDecoder decoder(buf);
  auto value = decoder.ReadUvarint32();
  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().code(), ErrorCode::ThriftDecodeError);
}

TEST(ParquetCorruptFileTest, FileReadPastEndIsAnError) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" /
                    "simple_test.parquet";
  ASSERT_OK_AND_ASSIGN(auto file, FileHandle::Open(path.string()));
  EXPECT_FALSE(file.Read(file.size() - 1, 2).has_value());
  EXPECT_FALSE(file.Read(SIZE_MAX, 1).has_value());
  EXPECT_FALSE(file.Read(0, SIZE_MAX).has_value());
}

// =============================================================================
// DATE Columns
// =============================================================================

TEST(ParquetDateTest, DictionaryEncodedDateLoadsAsDate32) {
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" /
                    "tpch" / "lineitem.parquet";
  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(path.string()));
  for (const char* name : {"l_commitdate", "l_receiptdate", "l_shipdate"}) {
    auto idx = ds.schema().GetFieldIndex(name);
    ASSERT_TRUE(idx.has_value()) << name;
    EXPECT_EQ(ds.schema().field(*idx).data_type, DataType::kDate32) << name;
  }

  ASSERT_OK_AND_ASSIGN(auto batch, ds.ReadPartition(0, {"l_commitdate"}));
  EXPECT_EQ(batch.column(0).GetType(), DataType::kDate32);
  EXPECT_EQ(batch.column(0).Get<Date32>(0), Date32{9538});
  EXPECT_EQ(FormatCell(batch.column(0), 0), "1996-02-12");
}

TEST(ParquetDateTest, PlainEncodedDateWithNullLoadsAsDate32) {
  // PLAIN encoding, converted_type DATE. Generated with DuckDB:
  //   COPY (SELECT * FROM (VALUES (DATE '1996-02-12'), (NULL), (DATE '1969-12-31'),
  //   (DATE '2000-02-29')) t(d)) TO 'testdata/date_plain.parquet'
  //   (FORMAT parquet, DICTIONARY_SIZE_LIMIT 1, COMPRESSION uncompressed);
  const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" /
                    "date_plain.parquet";
  ASSERT_OK_AND_ASSIGN(auto ds, ParquetDataSource::Open(path.string()));
  ASSERT_OK_AND_ASSIGN(auto batch, ds.ReadPartition(0, {"d"}));
  const auto& col = batch.column(0);
  EXPECT_EQ(col.GetType(), DataType::kDate32);
  ASSERT_EQ(col.size(), 4U);
  EXPECT_EQ(col.Get<Date32>(0), Date32{9538});
  EXPECT_TRUE(col.IsNull(1));
  EXPECT_EQ(FormatCell(col, 2), "1969-12-31");
  EXPECT_EQ(FormatCell(col, 3), "2000-02-29");
}

// =============================================================================
// TPC-H lineitem (testdata/tpch, written by DuckDB; 3 row groups)
// =============================================================================

namespace {

[[nodiscard]] std::string TpchLineitem() {
  return (std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "tpch" /
          "lineitem.parquet")
      .string();
}

}  // namespace

TEST(ParquetTpchTest, DecimalColumnReadsAsDecimal) {
  ASSERT_OK_AND_ASSIGN(auto reader, ParquetFileReader::Open(TpchLineitem()));
  const auto index = reader.schema().GetFieldIndex("l_extendedprice");
  ASSERT_TRUE(index.has_value());
  const auto& field = reader.schema().field(*index);
  EXPECT_EQ(field.data_type, DataType::Decimal(15, 2));
  EXPECT_FALSE(field.field_id.has_value());  // Thrift field 7 is the scale, not the field id

  ASSERT_OK_AND_ASSIGN(auto batch, reader.ReadRowGroup(0, {"l_extendedprice", "l_discount"}));
  EXPECT_EQ(batch.column(0).Get<Decimal64>(0).unscaled, 2471035);  // 24710.35
  EXPECT_EQ(batch.column(0).GetType().scale, 2);
  EXPECT_EQ(batch.column(1).Get<Decimal64>(0).unscaled, 4);        // 0.04
  EXPECT_EQ(batch.column(1).GetType().scale, 2);
}

TEST(ParquetTpchTest, ParallelRowGroupReadsMatchSerialReads) {
  ASSERT_OK_AND_ASSIGN(auto reader, ParquetFileReader::Open(TpchLineitem()));
  const std::size_t groups = reader.num_row_groups();
  ASSERT_GT(groups, 1U);
  std::vector<std::string> serial;
  for (std::size_t g = 0; g < groups; ++g) {
    ASSERT_OK_AND_ASSIGN(auto batch, reader.ReadRowGroup(g, {}));
    serial.push_back(batch.ToCSV());
  }

  constexpr int kThreads = 8;
  constexpr int kRounds = 4;
  std::vector<std::vector<std::string>> reads(kThreads);
  {
    std::vector<std::jthread> threads;
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&reader, &reads, groups, t] {
        for (int round = 0; round < kRounds; ++round) {
          for (std::size_t g = 0; g < groups; ++g) {
            auto batch = reader.ReadRowGroup(g, {});
            reads[t].push_back(batch ? batch->ToCSV() : batch.error().ToString());
          }
        }
      });
    }
  }
  for (const auto& thread_reads : reads) {
    ASSERT_EQ(thread_reads.size(), kRounds * groups);
    for (std::size_t i = 0; i < thread_reads.size(); ++i) {
      EXPECT_EQ(thread_reads[i], serial[i % groups]) << "read " << i;
    }
  }
}

// testdata/snappy_dictionary_page.parquet holds 100 l_shipmode rows of TPC-H SF 1 row group 2
// (DuckDB, Snappy). Its dictionary page is 58 bytes compressed and 58 bytes uncompressed.
TEST(ParquetSnappyTest, PageWithEqualSizesIsStillDecompressed) {
  ASSERT_OK_AND_ASSIGN(
      auto reader,
      ParquetFileReader::Open((std::filesystem::path(__FILE__).parent_path().parent_path() /
                               "testdata" / "snappy_dictionary_page.parquet")
                                  .string()));
  ASSERT_OK_AND_ASSIGN(auto batch, reader.ReadRowGroup(0, {}));
  ASSERT_EQ(batch.num_rows(), 100U);
  const std::vector<std::string> first = {"MAIL", "MAIL", "MAIL", "TRUCK",
                                          "RAIL", "RAIL", "SHIP", "FOB"};
  for (std::size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(batch.column(0).Get<std::string_view>(i), first[i]) << "row " << i;
  }
}

// =============================================================================
// Type mapping (testdata/types; generate.sql and generate_int96.py there)
// =============================================================================

namespace {

[[nodiscard]] std::string TypesFile(const std::string& name) {
  return (std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata" / "types" /
          name)
      .string();
}

}  // namespace

TEST(ParquetTypeMappingTest, TimestampUnitsUnsignedAndDecimal) {
  ASSERT_OK_AND_ASSIGN(auto reader,
                       ParquetFileReader::Open(TypesFile("timestamps_unsigned.parquet")));
  const std::vector<std::pair<std::string, DataType>> expected_fields = {
      {"ts_ms", DataType::kTimestamp}, {"ts_us", DataType::kTimestamp},
      {"ts_ns", DataType::kTimestamp}, {"u8", DataType::kInt32},
      {"u16", DataType::kInt32},       {"u32", DataType::kInt64},
      {"dec32", DataType::Decimal(9, 2)}};
  ASSERT_EQ(reader.schema().num_fields(), expected_fields.size());
  for (std::size_t i = 0; i < expected_fields.size(); ++i) {
    EXPECT_EQ(reader.schema().field(i).name, expected_fields[i].first);
    EXPECT_EQ(reader.schema().field(i).data_type, expected_fields[i].second)
        << expected_fields[i].first;
  }

  ASSERT_OK_AND_ASSIGN(auto batch, reader.ReadRowGroup(0, {}));
  auto text = [&batch](std::size_t column, std::size_t row) {
    return FormatCell(batch.column(column), row);
  };
  EXPECT_EQ(text(0, 0), "2024-01-02 03:04:05.5");
  EXPECT_EQ(text(1, 0), "2024-01-02 03:04:05.123456");
  EXPECT_EQ(text(2, 0), "2024-01-02 03:04:05.123456");  // nanoseconds drop to micros
  EXPECT_EQ(batch.column(3).Get<std::int32_t>(0), 200);
  EXPECT_EQ(batch.column(4).Get<std::int32_t>(0), 60000);
  EXPECT_EQ(batch.column(5).Get<std::int64_t>(0), 4000000000);  // above INT32_MAX
  EXPECT_EQ(FormatCell(batch.column(6), 0), "1234567.89");  // INT32 widened

  EXPECT_TRUE(batch.column(0).IsNull(1));
  EXPECT_EQ(text(1, 1), "1969-12-31 23:59:59.999999");
  EXPECT_EQ(text(2, 1), "1969-12-31 23:59:59.999999");  // -1 ns floors to -1 us
  EXPECT_TRUE(batch.column(5).IsNull(1));
  EXPECT_EQ(FormatCell(batch.column(6), 1), "-0.07");
}

TEST(ParquetTypeMappingTest, Int96ReadsAsTimestamp) {
  ASSERT_OK_AND_ASSIGN(auto reader, ParquetFileReader::Open(TypesFile("int96_timestamps.parquet")));
  ASSERT_EQ(reader.schema().num_fields(), 1U);
  EXPECT_EQ(reader.schema().field(0).data_type, DataType::kTimestamp);
  ASSERT_OK_AND_ASSIGN(auto batch, reader.ReadRowGroup(0, {}));
  ASSERT_EQ(batch.num_rows(), 3U);
  EXPECT_EQ(batch.column(0).Get<Timestamp>(0), Timestamp{0});
  EXPECT_EQ(FormatCell(batch.column(0), 1), "2024-01-02 03:04:05.5");
  EXPECT_EQ(batch.column(0).Get<Timestamp>(2), Timestamp{-1});
}

TEST(ParquetTypeMappingTest, UnsupportedColumnsFailToOpen) {
  for (const char* name : {"uint64.parquet", "uuid.parquet"}) {
    auto reader = ParquetFileReader::Open(TypesFile(name));
    ASSERT_FALSE(reader.has_value()) << name;
    EXPECT_EQ(reader.error().code(), ErrorCode::TypeError) << name;
  }
}

TEST(ParquetTypeMappingTest, DictionaryEncodedInt96ReadsAsTimestamp) {
  ASSERT_OK_AND_ASSIGN(auto reader, ParquetFileReader::Open(TypesFile("int96_dictionary.parquet")));
  ASSERT_OK_AND_ASSIGN(auto batch, reader.ReadRowGroup(0, {}));
  ASSERT_EQ(batch.num_rows(), 4U);
  const Timestamp later{1'704'164'645'500'000};
  EXPECT_EQ(batch.column(0).Get<Timestamp>(0), later);
  EXPECT_EQ(batch.column(0).Get<Timestamp>(1), Timestamp{-1});
  EXPECT_EQ(batch.column(0).Get<Timestamp>(2), later);
  EXPECT_EQ(batch.column(0).Get<Timestamp>(3), later);
}

TEST(ParquetTypeMappingTest, MillisOutOfMicrosRangeIsOutOfRange) {
  ASSERT_OK_AND_ASSIGN(auto reader,
                       ParquetFileReader::Open(TypesFile("timestamp_ms_infinity.parquet")));
  auto batch = reader.ReadRowGroup(0, {});
  ASSERT_FALSE(batch.has_value());
  EXPECT_EQ(batch.error().code(), ErrorCode::OutOfRange);
}
