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

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/record_batch_serializer.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/datasource/types.hpp"

using namespace quest::datasource;

// =============================================================================
// Basic Serialization Tests
// =============================================================================

TEST(RecordBatchSerializerTest, SerializeDeserializeInt32Column) {
  Schema schema({Field("id", Int32Type{})});

  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  col->Append(1);
  col->Append(2);
  col->Append(3);
  col->Append(4);
  col->Append(5);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  EXPECT_GT(data.size(), 0U);

  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

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
    Field("id", Int32Type{}),
    Field("name", StringType{}),
    Field("value", DoubleType{})
  });

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 3);
  id_col->Append(1);
  id_col->Append(2);
  id_col->Append(3);

  auto name_col = std::make_shared<StringColumnVector>(StringType{}, 3);
  name_col->Append("Alice");
  name_col->Append("Bob");
  name_col->Append("Charlie");

  auto value_col = std::make_shared<TypedColumnVector<double>>(DoubleType{}, 3);
  value_col->Append(3.14);
  value_col->Append(2.718);
  value_col->Append(1.414);

  std::vector<std::shared_ptr<ColumnVector>> columns = {id_col, name_col, value_col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

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
  Schema schema({Field("value", Int32Type{}, Repetition::kOptional)});

  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  col->Append(10);
  col->AppendNull();
  col->Append(20);
  col->AppendNull();
  col->Append(30);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_FALSE(deserialized.column(0).IsNull(0));
  EXPECT_TRUE(deserialized.column(0).IsNull(1));
  EXPECT_FALSE(deserialized.column(0).IsNull(2));
  EXPECT_TRUE(deserialized.column(0).IsNull(3));
  EXPECT_FALSE(deserialized.column(0).IsNull(4));

  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(0)), 10);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(2)), 20);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(4)), 30);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeStringWithNulls) {
  Schema schema({Field("text", StringType{}, Repetition::kOptional)});

  auto col = std::make_shared<StringColumnVector>(StringType{}, 4);
  col->Append("Hello");
  col->AppendNull();
  col->Append("World");
  col->AppendNull();

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

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
  Schema schema({Field("flag", BoolType{})});

  auto col = std::make_shared<TypedColumnVector<bool>>(BoolType{}, 4);
  col->Append(true);
  col->Append(false);
  col->Append(true);
  col->Append(true);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(0)), true);
  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(1)), false);
  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(2)), true);
  EXPECT_EQ(std::get<bool>(deserialized.column(0).GetValue(3)), true);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeInt64) {
  Schema schema({Field("big_number", Int64Type{})});

  auto col = std::make_shared<TypedColumnVector<int64_t>>(Int64Type{}, 3);
  col->Append(9223372036854775807LL);  // max int64
  col->Append(0);
  col->Append(-9223372036854775807LL);  // min int64 + 1

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_EQ(std::get<int64_t>(deserialized.column(0).GetValue(0)), 9223372036854775807LL);
  EXPECT_EQ(std::get<int64_t>(deserialized.column(0).GetValue(1)), 0);
  EXPECT_EQ(std::get<int64_t>(deserialized.column(0).GetValue(2)), -9223372036854775807LL);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeFloat) {
  Schema schema({Field("price", FloatType{})});

  auto col = std::make_shared<TypedColumnVector<float>>(FloatType{}, 3);
  col->Append(1.5f);
  col->Append(2.75f);
  col->Append(-0.125f);

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_FLOAT_EQ(std::get<float>(deserialized.column(0).GetValue(0)), 1.5f);
  EXPECT_FLOAT_EQ(std::get<float>(deserialized.column(0).GetValue(1)), 2.75f);
  EXPECT_FLOAT_EQ(std::get<float>(deserialized.column(0).GetValue(2)), -0.125f);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeAllIntTypes) {
  Schema schema({
    Field("int8_col", Int8Type{}),
    Field("int16_col", Int16Type{}),
    Field("int32_col", Int32Type{}),
    Field("int64_col", Int64Type{})
  });

  auto int8_col = std::make_shared<TypedColumnVector<int8_t>>(Int8Type{}, 2);
  int8_col->Append(127);
  int8_col->Append(-128);

  auto int16_col = std::make_shared<TypedColumnVector<int16_t>>(Int16Type{}, 2);
  int16_col->Append(32767);
  int16_col->Append(-32768);

  auto int32_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 2);
  int32_col->Append(2147483647);
  int32_col->Append(-2147483648);

  auto int64_col = std::make_shared<TypedColumnVector<int64_t>>(Int64Type{}, 2);
  int64_col->Append(9223372036854775807LL);
  int64_col->Append(-9223372036854775807LL);

  std::vector<std::shared_ptr<ColumnVector>> columns = {int8_col, int16_col, int32_col, int64_col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

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
    Field("id", Int32Type{}),
    Field("name", StringType{})
  });

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 3);
  id_col->Append(1);
  id_col->Append(2);
  id_col->Append(3);

  auto name_col = std::make_shared<StringColumnVector>(StringType{}, 3);
  name_col->Append("Test1");
  name_col->Append("Test2");
  name_col->Append("Test3");

  std::vector<std::shared_ptr<ColumnVector>> columns = {id_col, name_col};
  RecordBatch original(schema, columns);

  std::string filename = "/tmp/test_record_batch.bin";
  RecordBatchSerializer::SerializeToFile(original, filename);

  RecordBatch deserialized = RecordBatchSerializer::DeserializeFromFile(filename);

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
  Schema schema({Field("value", Int32Type{})});
  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 0);
  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_EQ(deserialized.num_rows(), 0U);
  EXPECT_EQ(deserialized.num_columns(), 1U);
}

TEST(RecordBatchSerializerTest, SerializeDeserializeEmptyStrings) {
  Schema schema({Field("text", StringType{})});

  auto col = std::make_shared<StringColumnVector>(StringType{}, 3);
  col->Append("");
  col->Append("non-empty");
  col->Append("");

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(0)), "");
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(1)), "non-empty");
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(2)), "");
}

TEST(RecordBatchSerializerTest, InvalidMagicNumber) {
  std::vector<uint8_t> invalid_data = {'X', 'X', 'X', 'X'};
  EXPECT_THROW(RecordBatchSerializer::Deserialize(invalid_data), std::runtime_error);
}

TEST(RecordBatchSerializerTest, InvalidVersion) {
  std::vector<uint8_t> data = {'Q', 'R', 'B', 'T'};
  uint32_t invalid_version = 999;
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(&invalid_version);
  data.insert(data.end(), ptr, ptr + sizeof(uint32_t));

  EXPECT_THROW(RecordBatchSerializer::Deserialize(data), std::runtime_error);
}

TEST(RecordBatchSerializerTest, TruncatedBuffer) {
  Schema schema({Field("value", Int32Type{})});
  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 1);
  col->Append(42);
  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);

  data.resize(10);

  EXPECT_THROW(RecordBatchSerializer::Deserialize(data), std::runtime_error);
}

// =============================================================================
// Large Data Tests
// =============================================================================

TEST(RecordBatchSerializerTest, LargeNumberOfRows) {
  Schema schema({Field("id", Int32Type{})});

  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 10000);
  for (int32_t i = 0; i < 10000; ++i) {
    col->Append(i);
  }

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_EQ(deserialized.num_rows(), 10000U);

  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(0)), 0);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(5000)), 5000);
  EXPECT_EQ(std::get<int32_t>(deserialized.column(0).GetValue(9999)), 9999);
}

TEST(RecordBatchSerializerTest, LongStrings) {
  Schema schema({Field("text", StringType{})});

  auto col = std::make_shared<StringColumnVector>(StringType{}, 2);
  std::string long_string(10000, 'A');
  col->Append(long_string);
  col->Append("short");

  std::vector<std::shared_ptr<ColumnVector>> columns = {col};
  RecordBatch original(schema, columns);

  std::vector<uint8_t> data = RecordBatchSerializer::Serialize(original);
  RecordBatch deserialized = RecordBatchSerializer::Deserialize(data);

  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(0)), long_string);
  EXPECT_EQ(std::get<std::string>(deserialized.column(0).GetValue(1)), "short");
}
