// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_record_batch_serializer.cpp
/// @brief Unit tests for RecordBatch serialization/deserialization.

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/record_batch_serializer.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "support/test_utils.hpp"

using namespace quest::columnar;

// =============================================================================
// Basic Serialization Tests
// =============================================================================

TEST(RecordBatchSerializerTest, SerializeDeserializeInt32Column) {
  Schema schema({Field("id", DataType::kInt32)});

  auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 5);
  col->Append(1);
  col->Append(2);
  col->Append(3);
  col->Append(4);
  col->Append(5);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  EXPECT_GT(data.size(), 0U);

  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(deserialized.num_rows(), original.num_rows());
  EXPECT_EQ(deserialized.num_columns(), original.num_columns());
  EXPECT_EQ(deserialized.schema().num_fields(), 1U);
  EXPECT_EQ(deserialized.schema().field(0).name, "id");

  for (size_t i = 0; i < 5; ++i) {
    EXPECT_FALSE(deserialized.column(0).IsNull(i));
    EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(i)),
              std::get<int32_t>(original.column(0).GetValue(i)));
  }
}

TEST(RecordBatchSerializerTest, SerializeDeserializeMultipleColumns) {
  Schema schema({
    Field("id", DataType::kInt32),
    Field("name", DataType::kString),
    Field("value", DataType::kDouble)
  });

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 3);
  id_col->Append(1);
  id_col->Append(2);
  id_col->Append(3);

  auto name_col = std::make_shared<StringColumnVector>(DataType::kString, 3);
  name_col->Append("Alice");
  name_col->Append("Bob");
  name_col->Append("Charlie");

  auto value_col = std::make_shared<TypedColumnVector<double>>(DataType::kDouble, 3);
  value_col->Append(3.14);
  value_col->Append(2.718);
  value_col->Append(1.414);

  std::vector<std::shared_ptr<ColumnVector>> columns = {id_col, name_col, value_col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(deserialized.num_rows(), 3U);
  EXPECT_EQ(deserialized.num_columns(), 3U);

  EXPECT_EQ(deserialized.schema().field(0).name, "id");
  EXPECT_EQ(deserialized.schema().field(1).name, "name");
  EXPECT_EQ(deserialized.schema().field(2).name, "value");

  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(i)),
              std::get<int32_t>(original.column(0).GetValue(i)));
    EXPECT_EQ(std::get<std::string>(deserialized.column(1).GetValue(i)),
              std::get<std::string>(original.column(1).GetValue(i)));
    EXPECT_DOUBLE_EQ(std::get<double>(deserialized.column(2).GetValue(i)),
                     std::get<double>(original.column(2).GetValue(i)));
  }
}

// =============================================================================
// Null Value Tests
// =============================================================================

TEST(RecordBatchSerializerTest, SerializeDeserializeWithNulls) {
  Schema schema({Field("value", DataType::kInt32, Repetition::kOptional)});

  auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 5);
  col->Append(10);
  col->AppendNull();
  col->Append(20);
  col->AppendNull();
  col->Append(30);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_FALSE(deserialized.column(0).IsNull(0));
  EXPECT_TRUE(deserialized.column(0).IsNull(1));
  EXPECT_FALSE(deserialized.column(0).IsNull(2));
  EXPECT_TRUE(deserialized.column(0).IsNull(3));
  EXPECT_FALSE(deserialized.column(0).IsNull(4));

  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(0)), 10);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(2)), 20);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(4)), 30);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeDate32WithNulls) {
  Schema schema({Field("d", DataType::kDate32, Repetition::kOptional)});
  auto col = std::make_shared<TypedColumnVector<Date32>>(DataType::kDate32, 3);
  col->Append(Date32{9538});
  col->AppendNull();
  col->Append(Date32{-1});
  RecordBatch original(schema, {col});

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(deserialized.schema().field(0).data_type, DataType::kDate32);
  const auto& out = deserialized.column(0);
  EXPECT_EQ(out.GetType(), DataType::kDate32);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_EQ(out.GetValue(0), Value{Date32{9538}});
  EXPECT_TRUE(out.IsNull(1));
  EXPECT_EQ(out.GetValue(2), Value{Date32{-1}});
}

TEST(RecordBatchSerializerTest, SerializeDeserializeTimestampWithNulls) {
  Schema schema({Field("t", DataType::kTimestamp, Repetition::kOptional)});
  auto col = std::make_shared<TypedColumnVector<Timestamp>>(DataType::kTimestamp, 3);
  col->Append(Timestamp{1'704'164'645'500'000});
  col->AppendNull();
  col->Append(Timestamp{-1});
  RecordBatch original(schema, {col});

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(deserialized.schema().field(0).data_type, DataType::kTimestamp);
  const auto& out = deserialized.column(0);
  ASSERT_EQ(out.size(), 3U);
  EXPECT_EQ(out.GetValue(0), Value{Timestamp{1'704'164'645'500'000}});
  EXPECT_TRUE(out.IsNull(1));
  EXPECT_EQ(out.GetValue(2), Value{Timestamp{-1}});
}

TEST(RecordBatchSerializerTest, SerializeDeserializeStringWithNulls) {
  Schema schema({Field("text", DataType::kString, Repetition::kOptional)});

  auto col = std::make_shared<StringColumnVector>(DataType::kString, 4);
  col->Append("Hello");
  col->AppendNull();
  col->Append("World");
  col->AppendNull();

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_FALSE(deserialized.column(0).IsNull(0));
  EXPECT_TRUE(deserialized.column(0).IsNull(1));
  EXPECT_FALSE(deserialized.column(0).IsNull(2));
  EXPECT_TRUE(deserialized.column(0).IsNull(3));

  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(0)), "Hello");
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(2)), "World");
}

// =============================================================================
// Different Data Type Tests
// =============================================================================

TEST(RecordBatchSerializerTest, SerializeDeserializeBool) {
  Schema schema({Field("flag", DataType::kBool)});

  auto col = std::make_shared<TypedColumnVector<bool>>(DataType::kBool, 4);
  col->Append(true);
  col->Append(false);
  col->Append(true);
  col->Append(true);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(0)), true);
  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(1)), false);
  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(2)), true);
  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(3)), true);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeInt64) {
  Schema schema({Field("big_number", DataType::kInt64)});

  auto col = std::make_shared<TypedColumnVector<int64_t>>(DataType::kInt64, 3);
  col->Append(9223372036854775807LL);  // max int64
  col->Append(0);
  col->Append(-9223372036854775807LL);  // min int64 + 1

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(std::get<int64_t>(deserialized.column(0).GetValue(0)), 9223372036854775807LL);
  EXPECT_EQ(std::get<int64_t>(deserialized.column(0).GetValue(1)), 0);
  EXPECT_EQ(std::get<int64_t>(deserialized.column(0).GetValue(2)), -9223372036854775807LL);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeFloat) {
  Schema schema({Field("price", DataType::kFloat)});

  auto col = std::make_shared<TypedColumnVector<float>>(DataType::kFloat, 3);
  col->Append(1.5f);
  col->Append(2.75f);
  col->Append(-0.125f);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_FLOAT_EQ(std::get<float>(deserialized.column(0).GetValue(0)), 1.5f);
  EXPECT_FLOAT_EQ(std::get<float>(deserialized.column(0).GetValue(1)), 2.75f);
  EXPECT_FLOAT_EQ(std::get<float>(deserialized.column(0).GetValue(2)), -0.125f);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeAllIntTypes) {
  Schema schema({
    Field("int8_col", DataType::kInt8),
    Field("int16_col", DataType::kInt16),
    Field("int32_col", DataType::kInt32),
    Field("int64_col", DataType::kInt64)
  });

  auto int8_col = std::make_shared<TypedColumnVector<int8_t>>(DataType::kInt8, 2);
  int8_col->Append(127);
  int8_col->Append(-128);

  auto int16_col = std::make_shared<TypedColumnVector<int16_t>>(DataType::kInt16, 2);
  int16_col->Append(32767);
  int16_col->Append(-32768);

  auto int32_col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 2);
  int32_col->Append(2147483647);
  int32_col->Append(-2147483648);

  auto int64_col = std::make_shared<TypedColumnVector<int64_t>>(DataType::kInt64, 2);
  int64_col->Append(9223372036854775807LL);
  int64_col->Append(-9223372036854775807LL);

  std::vector<std::shared_ptr<ColumnVector>> columns = {int8_col, int16_col, int32_col, int64_col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(std::get<int8_t>(deserialized.column(0).GetValue(0)), 127);
  EXPECT_EQ(std::get<int8_t>(deserialized.column(0).GetValue(1)), -128);
  EXPECT_EQ(std::get<int16_t>(deserialized.column(1).GetValue(0)), 32767);
  EXPECT_EQ(std::get<int16_t>(deserialized.column(1).GetValue(1)), -32768);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(2).GetValue(0)), 2147483647);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(2).GetValue(1)), -2147483648);
  EXPECT_EQ(std::get<int64_t>(deserialized.column(3).GetValue(0)), 9223372036854775807LL);
  EXPECT_EQ(std::get<int64_t>(deserialized.column(3).GetValue(1)), -9223372036854775807LL);
}

// =============================================================================
// File I/O Tests
// =============================================================================

TEST(RecordBatchSerializerTest, SerializeDeserializeFile) {
  Schema schema({
    Field("id", DataType::kInt32),
    Field("name", DataType::kString)
  });

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 3);
  id_col->Append(1);
  id_col->Append(2);
  id_col->Append(3);

  auto name_col = std::make_shared<StringColumnVector>(DataType::kString, 3);
  name_col->Append("Test1");
  name_col->Append("Test2");
  name_col->Append("Test3");

  std::vector<std::shared_ptr<ColumnVector>> columns = {id_col, name_col};
  RecordBatch original(schema, columns);

  std::string filename = "/tmp/test_record_batch.bin";
  ASSERT_OK(RecordBatchSerializer::SerializeToFile(original, filename));

  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized,
                       RecordBatchSerializer::DeserializeFromFile(filename));

  EXPECT_EQ(deserialized.num_rows(), original.num_rows());
  EXPECT_EQ(deserialized.num_columns(), original.num_columns());

  for (size_t i = 0; i < 3; ++i) {
    EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(i)),
              std::get<int32_t>(original.column(0).GetValue(i)));
    EXPECT_EQ(std::get<std::string>(deserialized.column(1).GetValue(i)),
              std::get<std::string>(original.column(1).GetValue(i)));
  }

  std::filesystem::remove(filename);
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(RecordBatchSerializerTest, SerializeDeserializeEmptyBatch) {
  Schema schema({Field("value", DataType::kInt32)});
  auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 0);
  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(deserialized.num_rows(), 0U);
  EXPECT_EQ(deserialized.num_columns(), 1U);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeEmptyStrings) {
  Schema schema({Field("text", DataType::kString)});

  auto col = std::make_shared<StringColumnVector>(DataType::kString, 3);
  col->Append("");
  col->Append("non-empty");
  col->Append("");

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(0)), "");
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(1)), "non-empty");
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(2)), "");
}

TEST(RecordBatchSerializerTest, InvalidMagicNumber) {
  std::vector<uint8_t> invalid_data = {'X', 'X', 'X', 'X'};
  auto result = RecordBatchSerializer::Deserialize(invalid_data);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), quest::ErrorCode::Invalid);
}

TEST(RecordBatchSerializerTest, InvalidVersion) {
  std::vector<uint8_t> data = {'Q', 'R', 'B', 'T'};
  uint32_t invalid_version = 999;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&invalid_version);
  data.insert(data.end(), ptr, ptr + sizeof(uint32_t));

  auto result = RecordBatchSerializer::Deserialize(data);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), quest::ErrorCode::Invalid);
}

TEST(RecordBatchSerializerTest, TruncatedBuffer) {
  Schema schema({Field("value", DataType::kInt32)});
  auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 1);
  col->Append(42);
  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));

  data.resize(10);

  auto result = RecordBatchSerializer::Deserialize(data);
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), quest::ErrorCode::Invalid);
}

// =============================================================================
// Large Data Tests
// =============================================================================

TEST(RecordBatchSerializerTest, LargeNumberOfRows) {
  Schema schema({Field("id", DataType::kInt32)});

  auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, 10000);
  for (int32_t i = 0; i < 10000; ++i) {
    col->Append(i);
  }

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(deserialized.num_rows(), 10000U);

  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(0)), 0);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(5000)), 5000);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(9999)), 9999);
}

TEST(RecordBatchSerializerTest, LongStrings) {
  Schema schema({Field("text", DataType::kString)});

  auto col = std::make_shared<StringColumnVector>(DataType::kString, 2);
  std::string long_string(10000, 'A');
  col->Append(long_string);
  col->Append("short");

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  ASSERT_OK_AND_ASSIGN(std::vector<uint8_t> data, RecordBatchSerializer::Serialize(original));
  ASSERT_OK_AND_ASSIGN(RecordBatch deserialized, RecordBatchSerializer::Deserialize(data));

  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(0)), long_string);
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(1)), "short");
}
