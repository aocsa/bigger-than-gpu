// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_column_vector.cpp
/// @brief ColumnBuilder, ColumnVector and ColumnView: layout, nulls, constants, Value bridge.

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "support/test_utils.hpp"

using namespace quest::columnar;
using quest::testing::MakeColumn;

// =============================================================================
// Fixed-width columns
// =============================================================================

TEST(ColumnBuilderTest, Int32ValuesWithoutNullsHaveNoBitmap) {
  ColumnBuilder<int32_t> builder(DataType::kInt32, 3);
  builder.Append(10);
  builder.Append(-20);
  builder.Append(30);
  EXPECT_EQ(builder.size(), 3U);
  const auto col = std::move(builder).Finish();

  EXPECT_EQ(col->GetType(), DataType::kInt32);
  EXPECT_EQ(col->size(), 3U);
  EXPECT_EQ(col->null_count(), 0U);
  EXPECT_FALSE(col->is_constant());
  EXPECT_EQ(col->Get<int32_t>(1), -20);
  EXPECT_FALSE(col->IsNull(2));
  EXPECT_EQ(col->size_in_bytes(), 3 * sizeof(int32_t));  // values only

  const auto view = col->View<int32_t>();
  EXPECT_FALSE(view.has_nulls());
  EXPECT_EQ(view.size(), 3U);
  EXPECT_EQ(view[0] + view[2], 40);
}

TEST(ColumnBuilderTest, FirstNullMakesTheBitmapAndKeepsEarlierRowsValid) {
  // The first null comes after a full byte of valid rows, and rows follow it across the next
  // byte boundary.
  ColumnBuilder<int64_t> builder(DataType::kInt64);
  for (int64_t i = 0; i < 20; ++i) {
    i == 9 || i == 17 ? builder.AppendNull() : builder.Append(i);
  }
  const auto col = std::move(builder).Finish();

  EXPECT_EQ(col->null_count(), 2U);
  for (int64_t i = 0; i < 20; ++i) {
    const bool is_null = i == 9 || i == 17;
    EXPECT_EQ(col->IsNull(i), is_null) << "row " << i;
    if (!is_null) {
      EXPECT_EQ(col->Get<int64_t>(i), i);
    }
  }
  EXPECT_EQ(col->size_in_bytes(), 20 * sizeof(int64_t) + 3);  // values + 3 bitmap bytes
  EXPECT_TRUE(col->View<int64_t>().has_nulls());
}

TEST(ColumnBuilderTest, AllNullsAndEmptyColumns) {
  auto nulls = MakeColumn<double>(DataType::kDouble, {std::nullopt, std::nullopt, std::nullopt});
  EXPECT_EQ(nulls->size(), 3U);
  EXPECT_EQ(nulls->null_count(), 3U);
  EXPECT_TRUE(nulls->IsNull(0));
  EXPECT_TRUE(nulls->IsNull(2));

  auto empty = ColumnBuilder<float>(DataType::kFloat).Finish();
  EXPECT_EQ(empty->size(), 0U);
  EXPECT_EQ(empty->null_count(), 0U);
  EXPECT_EQ(empty->size_in_bytes(), 0U);
}

TEST(ColumnBuilderTest, BoolIsOneBytePerRow) {
  auto col = MakeColumn<bool>(DataType::kBool, {true, false, std::nullopt, true});
  EXPECT_EQ(col->size_in_bytes(), 4U + 1U);  // 4 value bytes + 1 bitmap byte
  const auto view = col->View<bool>();
  EXPECT_TRUE(view[0]);
  EXPECT_FALSE(view[1]);
  EXPECT_TRUE(view.IsNull(2));
  EXPECT_TRUE(view[3]);
}

TEST(ColumnBuilderTest, DoubleKeepsSpecialValues) {
  auto col = MakeColumn<double>(DataType::kDouble, {std::numeric_limits<double>::infinity(),
                                                    std::numeric_limits<double>::quiet_NaN(),
                                                    -0.0});
  EXPECT_TRUE(std::isinf(col->Get<double>(0)));
  EXPECT_TRUE(std::isnan(col->Get<double>(1)));
  EXPECT_TRUE(std::signbit(col->Get<double>(2)));
  EXPECT_FALSE(col->IsNull(1));  // NaN is a value, not a null
}

TEST(ColumnBuilderTest, Date32AndTimestamp) {
  auto dates = MakeColumn<Date32>(DataType::kDate32, {Date32{8766}, std::nullopt});
  EXPECT_EQ(dates->Get<Date32>(0), Date32{8766});
  EXPECT_TRUE(dates->IsNull(1));

  auto times = MakeColumn<Timestamp>(DataType::kTimestamp, {Timestamp{-1}});
  EXPECT_EQ(times->Get<Timestamp>(0), Timestamp{-1});
  EXPECT_EQ(times->size_in_bytes(), sizeof(Timestamp));
}

// =============================================================================
// String columns
// =============================================================================

TEST(StringColumnTest, OffsetsAndCharsHoldEveryRow) {
  auto col = MakeColumn<std::string_view>(DataType::kString,
                                          {"MAIL", std::nullopt, "", "SHIP"});
  EXPECT_EQ(col->size(), 4U);
  EXPECT_EQ(col->null_count(), 1U);
  EXPECT_EQ(col->Get<std::string_view>(0), "MAIL");
  EXPECT_TRUE(col->IsNull(1));
  EXPECT_FALSE(col->IsNull(2));  // an empty string is not a null
  EXPECT_EQ(col->Get<std::string_view>(2), "");
  EXPECT_EQ(col->Get<std::string_view>(3), "SHIP");
  // 8 chars, 5 int64 offsets, 1 bitmap byte.
  EXPECT_EQ(col->size_in_bytes(), 8U + 5 * sizeof(int64_t) + 1U);
}

TEST(StringColumnTest, KeepsBytesUnicodeAndLongStrings) {
  const std::string long_string(10'000, 'x');
  const std::string bytes("a\0b\xff", 4);
  auto col = MakeColumn<std::string_view>(DataType::kString,
                                          {"héllo wörld 你好", long_string, bytes});
  EXPECT_EQ(col->Get<std::string_view>(0), "héllo wörld 你好");
  EXPECT_EQ(col->Get<std::string_view>(1), long_string);
  EXPECT_EQ(col->Get<std::string_view>(2), bytes);
}

// =============================================================================
// Constant columns (literals)
// =============================================================================

TEST(ConstantColumnTest, OneRowStandsForEveryRowAndSharesBuffers) {
  auto one_row = MakeColumn<std::string_view>(DataType::kString, {"MAIL"});
  auto col = ColumnVector::Constant(*one_row, 1000);

  EXPECT_TRUE(col->is_constant());
  EXPECT_EQ(col->size(), 1000U);
  EXPECT_EQ(col->null_count(), 0U);
  EXPECT_EQ(col->size_in_bytes(), one_row->size_in_bytes());
  const auto view = col->View<std::string_view>();
  EXPECT_EQ(view[0], "MAIL");
  EXPECT_EQ(view[999], "MAIL");
  EXPECT_EQ(col->GetValue(500), Value{std::string("MAIL")});
}

TEST(ConstantColumnTest, NullConstantIsNullInEveryRow) {
  auto one_row = MakeColumn<int32_t>(DataType::kInt32, {std::nullopt});
  auto col = ColumnVector::Constant(*one_row, 4);
  EXPECT_EQ(col->null_count(), 4U);
  EXPECT_TRUE(col->IsNull(0));
  EXPECT_TRUE(col->IsNull(3));
  EXPECT_TRUE(col->View<int32_t>().IsNull(3));
}

TEST(ConstantColumnTest, NullConstantOfZeroRowsIsStillNull) {
  auto one_row = MakeColumn<int32_t>(DataType::kInt32, {std::nullopt});
  auto col = ColumnVector::Constant(*one_row, 0);
  EXPECT_EQ(col->size(), 0U);
  EXPECT_TRUE(col->IsNull(0));  // how a NULL literal is recognised on an empty batch
}

// =============================================================================
// Value bridge (until the Value variant goes)
// =============================================================================

TEST(ValueBridgeTest, GetValueGivesTheTypedAlternative) {
  auto ints = MakeColumn<int16_t>(DataType::kInt16, {7, std::nullopt});
  EXPECT_EQ(ints->GetValue(0), Value{int16_t{7}});
  EXPECT_TRUE(std::holds_alternative<std::monostate>(ints->GetValue(1)));
  EXPECT_TRUE(std::holds_alternative<std::monostate>(ints->GetValue(2)));  // past the end

  auto strings = MakeColumn<std::string_view>(DataType::kString, {"abc"});
  EXPECT_EQ(strings->GetValue(0), Value{std::string("abc")});
}

TEST(ValueBridgeTest, ColumnFromValuesChecksTypes) {
  const std::vector<Value> values = {Value{int32_t{1}}, Value{}, Value{int32_t{3}}};
  ASSERT_OK_AND_ASSIGN(auto col, ColumnFromValues(DataType::kInt32, values));
  EXPECT_EQ(col->Get<int32_t>(2), 3);
  EXPECT_TRUE(col->IsNull(1));

  const std::vector<Value> mixed = {Value{int32_t{1}}, Value{std::string("x")}};
  auto bad = ColumnFromValues(DataType::kInt32, mixed);
  ASSERT_FALSE(bad.has_value());
  EXPECT_EQ(bad.error().code(), quest::ErrorCode::TypeError);
}

// =============================================================================
// Date32 and Timestamp
// =============================================================================

TEST(Date32Test, ValueToStringIsIsoDate) {
  EXPECT_EQ(ValueToString(Value{Date32{9538}}), "1996-02-12");
  EXPECT_EQ(ValueToString(Value{Date32{0}}), "1970-01-01");
  EXPECT_EQ(ValueToString(Value{Date32{-1}}), "1969-12-31");
  EXPECT_EQ(ValueToString(Value{Date32{11016}}), "2000-02-29");
}

TEST(TimestampTest, FormatsLikeDuckDb) {
  EXPECT_EQ(ValueToString(Value{Timestamp{0}}), "1970-01-01 00:00:00");
  EXPECT_EQ(ValueToString(Value{Timestamp{1'704'164'645'500'000}}), "2024-01-02 03:04:05.5");
  EXPECT_EQ(ValueToString(Value{Timestamp{1'704'164'645'123'456}}), "2024-01-02 03:04:05.123456");
  EXPECT_EQ(ValueToString(Value{Timestamp{-1}}), "1969-12-31 23:59:59.999999");
  EXPECT_EQ(GetTypeName(DataType::kTimestamp), "Timestamp");
}

TEST(Date32Test, TypeTraits) {
  EXPECT_EQ(GetTypeName(DataType::kDate32), "Date32");
  EXPECT_TRUE(VisitType(DataType::kDate32,
                        []<typename Tag>(Tag) { return std::same_as<typename Tag::CType, Date32>; }));
}

TEST(Date32Test, ConcatenateKeepsDate32) {
  Schema schema({Field("d", DataType::kDate32, Repetition::kOptional)});
  RecordBatch a(schema, {MakeColumn<Date32>(DataType::kDate32, {Date32{1}})});
  RecordBatch b(schema, {MakeColumn<Date32>(DataType::kDate32, {std::nullopt, Date32{2}})});

  ASSERT_OK_AND_ASSIGN(auto merged, ConcatenateRecordBatches({&a, &b}));
  ASSERT_EQ(merged.num_rows(), 3U);
  EXPECT_EQ(merged.column(0).GetType(), DataType::kDate32);
  EXPECT_EQ(merged.column(0).GetValue(0), Value{Date32{1}});
  EXPECT_TRUE(merged.column(0).IsNull(1));
  EXPECT_EQ(merged.column(0).GetValue(2), Value{Date32{2}});
}

TEST(ConcatenateTest, StringsAcrossBatchesKeepOffsets) {
  Schema schema({Field("s", DataType::kString, Repetition::kOptional)});
  RecordBatch a(schema, {MakeColumn<std::string_view>(DataType::kString, {"ab", std::nullopt})});
  RecordBatch b(schema, {MakeColumn<std::string_view>(DataType::kString, {"", "cde"})});

  ASSERT_OK_AND_ASSIGN(auto merged, ConcatenateRecordBatches({&a, &b}));
  const auto view = merged.column(0).View<std::string_view>();
  ASSERT_EQ(view.size(), 4U);
  EXPECT_EQ(view[0], "ab");
  EXPECT_TRUE(view.IsNull(1));
  EXPECT_EQ(view[2], "");
  EXPECT_EQ(view[3], "cde");
}
