// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_parquet_golden.cpp
/// @brief Golden dumps of the parsed footer (and the page headers of small files) of every
/// Parquet file under testdata/, compared byte for byte with testdata/golden/.
///
/// To regenerate after an intended change: QUEST_UPDATE_GOLDEN=1 ./build/tests/test_parquet_golden

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "quest/io/parquet/metadata.hpp"
#include "quest/io/parquet/page_reader.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::io::parquet;

namespace {

const std::filesystem::path kTestData =
    std::filesystem::path(__FILE__).parent_path().parent_path() / "testdata";

/// Files below this size also get their page headers dumped.
constexpr std::uintmax_t kPageDumpMaxFileSize = 64 * 1024;

/// @brief Reads a whole file.
[[nodiscard]] std::vector<uint8_t> ReadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

/// @brief Quotes @p bytes, escaping quotes, backslashes and non-printable bytes as \xHH.
[[nodiscard]] std::string Quote(std::string_view bytes) {
  std::string out = "\"";
  for (const char c : bytes) {
    const auto byte = static_cast<unsigned char>(c);
    if (byte < 0x20 || byte >= 0x7F || c == '"' || c == '\\') {
      out += std::format("\\x{:02x}", byte);
    } else {
      out += c;
    }
  }
  return out + "\"";
}

/// @brief Hex digits of @p bytes.
[[nodiscard]] std::string Hex(const std::vector<uint8_t>& bytes) {
  std::string out;
  for (const uint8_t byte : bytes) {
    out += std::format("{:02x}", byte);
  }
  return out;
}

template <typename T>
[[nodiscard]] std::string Num(T value) {
  if constexpr (std::is_enum_v<T>) {
    return std::to_string(static_cast<int>(value));
  } else {
    return std::to_string(value);
  }
}

template <typename T>
[[nodiscard]] std::string Opt(const std::optional<T>& value) {
  if (!value) {
    return "-";
  }
  if constexpr (std::is_same_v<T, std::string>) {
    return Quote(*value);
  } else if constexpr (std::is_same_v<T, std::vector<uint8_t>>) {
    return Hex(*value);
  } else {
    return Num(*value);
  }
}

void DumpPageHeader(const PageHeader& h, std::ostringstream& out) {
  out << "type=" << Num(h.type) << " uncompressed_page_size=" << h.uncompressed_page_size
      << " compressed_page_size=" << h.compressed_page_size << " crc=" << h.crc
      << " num_values=" << h.num_values << " encoding=" << Num(h.encoding)
      << " definition_level_encoding=" << Num(h.definition_level_encoding)
      << " repetition_level_encoding=" << Num(h.repetition_level_encoding)
      << " num_nulls=" << h.num_nulls << " num_rows=" << h.num_rows
      << " definition_levels_byte_length=" << h.definition_levels_byte_length
      << " repetition_levels_byte_length=" << h.repetition_levels_byte_length
      << " is_compressed=" << h.is_compressed << " num_dict_values=" << h.num_dict_values
      << " dict_encoding=" << Num(h.dict_encoding) << "\n";
}

/// @brief Dumps the page headers of one column chunk, or the error that stopped the walk.
void DumpPages(std::span<const uint8_t> file, const ColumnChunkMetadata& cc,
               std::ostringstream& out) {
  const int64_t start = cc.dictionary_page_offset.value_or(cc.data_page_offset);
  PageReader reader(
      file.subspan(static_cast<size_t>(start), static_cast<size_t>(cc.total_compressed_size)));
  for (int page = 0; reader.HasNextPage(); ++page) {
    out << "      page[" << page << "]: ";
    auto header = reader.ReadPageHeader();
    if (!header) {
      out << "error " << header.error().ToString() << "\n";
      return;
    }
    DumpPageHeader(*header, out);
    if (auto data = reader.GetPageData(); !data) {
      out << "      error " << data.error().ToString() << "\n";
      return;
    }
  }
}

/// @brief Deterministic text of every FileMetadata field (and page headers if @p with_pages).
[[nodiscard]] std::string DumpFile(std::span<const uint8_t> file, bool with_pages) {
  std::ostringstream out;
  if (file.size() < 12) {
    return "file too short\n";
  }
  uint32_t footer_length = 0;
  std::memcpy(&footer_length, file.data() + file.size() - 8, sizeof(footer_length));
  auto parsed = MetadataParser::Parse(file.subspan(file.size() - 8 - footer_length, footer_length));
  if (!parsed) {
    return "error " + parsed.error().ToString() + "\n";
  }
  const FileMetadata& md = *parsed;
  out << "version=" << md.version << "\n";
  out << "num_rows=" << md.num_rows << "\n";
  out << "created_by=" << Opt(md.created_by) << "\n";
  for (size_t i = 0; i < md.schema_elements.size(); ++i) {
    const auto& e = md.schema_elements[i];
    out << "schema[" << i << "]: name=" << Quote(e.name) << " type=" << Num(e.type)
        << " type_length=" << Opt(e.type_length) << " repetition=" << Num(e.repetition)
        << " num_children=" << e.num_children << " logical_type=" << Opt(e.logical_type)
        << " precision=" << Opt(e.precision) << " scale=" << Opt(e.scale)
        << " field_id=" << Opt(e.field_id) << " time_unit=" << Opt(e.time_unit)
        << " bit_width=" << Opt(e.bit_width) << " is_signed=" << Opt(e.is_signed) << "\n";
  }
  for (size_t i = 0; i < md.key_value_metadata.size(); ++i) {
    const auto& kv = md.key_value_metadata[i];
    out << "key_value[" << i << "]: key=" << Quote(kv.key) << " value=" << Opt(kv.value) << "\n";
  }
  for (size_t g = 0; g < md.row_groups.size(); ++g) {
    const auto& rg = md.row_groups[g];
    out << "row_group[" << g << "]: num_rows=" << rg.num_rows
        << " total_byte_size=" << rg.total_byte_size << " file_offset=" << Opt(rg.file_offset)
        << " total_compressed_size=" << Opt(rg.total_compressed_size)
        << " ordinal=" << Opt(rg.ordinal) << "\n";
    for (size_t c = 0; c < rg.columns.size(); ++c) {
      const auto& cc = rg.columns[c];
      out << "  column[" << c << "]: file_path=" << Quote(cc.file_path)
          << " file_offset=" << cc.file_offset << " type=" << Num(cc.type) << " encodings=[";
      for (size_t k = 0; k < cc.encodings.size(); ++k) {
        out << (k ? "," : "") << Num(cc.encodings[k]);
      }
      out << "] path_in_schema=[";
      for (size_t k = 0; k < cc.path_in_schema.size(); ++k) {
        out << (k ? "," : "") << Quote(cc.path_in_schema[k]);
      }
      out << "] codec=" << Num(cc.codec) << " num_values=" << cc.num_values
          << " total_uncompressed_size=" << cc.total_uncompressed_size
          << " total_compressed_size=" << cc.total_compressed_size
          << " data_page_offset=" << cc.data_page_offset
          << " index_page_offset=" << Opt(cc.index_page_offset)
          << " dictionary_page_offset=" << Opt(cc.dictionary_page_offset) << "\n";
      const auto& s = cc.statistics;
      out << "    statistics: min=" << Opt(s.min_value) << " max=" << Opt(s.max_value)
          << " null_count=" << Opt(s.null_count) << " distinct_count=" << Opt(s.distinct_count)
          << " num_values=" << Opt(s.num_values) << "\n";
      if (with_pages) {
        DumpPages(file, cc, out);
      }
    }
  }
  return out.str();
}

/// @brief Every .parquet file under testdata/, relative to it, sorted.
[[nodiscard]] std::vector<std::filesystem::path> ParquetFiles() {
  std::vector<std::filesystem::path> files;
  for (const auto& entry : std::filesystem::recursive_directory_iterator(kTestData)) {
    if (entry.is_regular_file() && entry.path().extension() == ".parquet") {
      files.push_back(std::filesystem::relative(entry.path(), kTestData));
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

}  // namespace

TEST(ParquetGoldenTest, FootersMatchGoldenDumps) {
  const char* update_env = std::getenv("QUEST_UPDATE_GOLDEN");
  const bool update = update_env != nullptr && std::string_view(update_env) == "1";
  const auto files = ParquetFiles();
  ASSERT_GE(files.size(), 19U);
  for (const auto& relative : files) {
    SCOPED_TRACE(relative.string());
    const auto bytes = ReadFile(kTestData / relative);
    const std::string dump = DumpFile(bytes, bytes.size() < kPageDumpMaxFileSize);
    auto golden_path = kTestData / "golden" / relative;
    golden_path.replace_extension(".txt");
    if (update) {
      std::filesystem::create_directories(golden_path.parent_path());
      std::ofstream(golden_path, std::ios::binary) << dump;
      continue;
    }
    const auto golden_bytes = ReadFile(golden_path);
    const std::string golden(golden_bytes.begin(), golden_bytes.end());
    if (dump == golden) {
      continue;
    }
    std::istringstream dump_lines(dump);
    std::istringstream golden_lines(golden);
    std::string dump_line;
    std::string golden_line;
    for (int line = 1;; ++line) {
      const bool more_dump = static_cast<bool>(std::getline(dump_lines, dump_line));
      const bool more_golden = static_cast<bool>(std::getline(golden_lines, golden_line));
      if (more_dump != more_golden || dump_line != golden_line) {
        ADD_FAILURE() << golden_path << " differs at line " << line
                      << "\n  golden: " << (more_golden ? golden_line : "<eof>")
                      << "\n  actual: " << (more_dump ? dump_line : "<eof>");
        break;
      }
      if (!more_dump) {
        ADD_FAILURE() << golden_path << " differs (line endings)";
        break;
      }
    }
  }
}
