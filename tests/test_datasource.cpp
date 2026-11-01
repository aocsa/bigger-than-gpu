// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_datasource.cpp
/// @brief Unit tests for datasource module: Schema, ColumnVector, RecordBatch.

#include <gtest/gtest.h>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"
#include "quest/io/parquet/format.hpp"
#include "support/test_utils.hpp"

using namespace quest::columnar;
using namespace quest::io::parquet;

// =============================================================================
// Types Tests
// =============================================================================

TEST(TypesTest, PhysicalTypeToDataType) {
  auto bool_type = PhysicalTypeToDataType(PhysicalType::kBoolean);
  EXPECT_TRUE(std::holds_alternative<BoolType>(bool_type));

  auto int32_type = PhysicalTypeToDataType(PhysicalType::kInt32);
  EXPECT_TRUE(std::holds_alternative<Int32Type>(int32_type));

  auto int64_type = PhysicalTypeToDataType(PhysicalType::kInt64);
  EXPECT_TRUE(std::holds_alternative<Int64Type>(int64_type));

  auto float_type = PhysicalTypeToDataType(PhysicalType::kFloat);
  EXPECT_TRUE(std::holds_alternative<FloatType>(float_type));

  auto double_type = PhysicalTypeToDataType(PhysicalType::kDouble);
  EXPECT_TRUE(std::holds_alternative<DoubleType>(double_type));

  auto binary_type = PhysicalTypeToDataType(PhysicalType::kByteArray);
  EXPECT_TRUE(std::holds_alternative<BinaryType>(binary_type));

  auto fixed_type = PhysicalTypeToDataType(PhysicalType::kFixedLenByteArray, 16);
  EXPECT_TRUE(std::holds_alternative<FixedLenByteArrayType>(fixed_type));
  EXPECT_EQ(std::get<FixedLenByteArrayType>(fixed_type).byte_width, 16);
}

TEST(TypesTest, GetTypeName) {
  EXPECT_EQ(GetTypeName(BoolType{}), "Bool");
  EXPECT_EQ(GetTypeName(Int32Type{}), "Int32");
  EXPECT_EQ(GetTypeName(Int64Type{}), "Int64");
  EXPECT_EQ(GetTypeName(FloatType{}), "Float");
  EXPECT_EQ(GetTypeName(DoubleType{}), "Double");
  EXPECT_EQ(GetTypeName(StringType{}), "String");
  EXPECT_EQ(GetTypeName(BinaryType{}), "Binary");
  EXPECT_EQ(GetTypeName(FixedLenByteArrayType{16}), "FixedLenByteArray(16)");
}

TEST(TypesTest, GetTypeByteWidth) {
  EXPECT_EQ(GetTypeByteWidth(BoolType{}), 1);
  EXPECT_EQ(GetTypeByteWidth(Int8Type{}), 1);
  EXPECT_EQ(GetTypeByteWidth(Int16Type{}), 2);
  EXPECT_EQ(GetTypeByteWidth(Int32Type{}), 4);
  EXPECT_EQ(GetTypeByteWidth(Int64Type{}), 8);
  EXPECT_EQ(GetTypeByteWidth(FloatType{}), 4);
  EXPECT_EQ(GetTypeByteWidth(DoubleType{}), 8);
  EXPECT_EQ(GetTypeByteWidth(Int96Type{}), 12);
  EXPECT_EQ(GetTypeByteWidth(FixedLenByteArrayType{16}), 16);
  EXPECT_EQ(GetTypeByteWidth(StringType{}), -1);
  EXPECT_EQ(GetTypeByteWidth(BinaryType{}), -1);
}

TEST(TypesTest, IsFixedWidth) {
  EXPECT_TRUE(IsFixedWidth(Int32Type{}));
  EXPECT_TRUE(IsFixedWidth(Int64Type{}));
  EXPECT_TRUE(IsFixedWidth(DoubleType{}));
  EXPECT_FALSE(IsFixedWidth(StringType{}));
  EXPECT_FALSE(IsFixedWidth(BinaryType{}));
}

// =============================================================================
// Field Tests
// =============================================================================

TEST(FieldTest, Construction) {
  Field f1("id", Int32Type{});
  EXPECT_EQ(f1.name, "id");
  EXPECT_TRUE(std::holds_alternative<Int32Type>(f1.data_type));
  EXPECT_EQ(f1.repetition, Repetition::kRequired);
  EXPECT_FALSE(f1.IsNullable());

  Field f2("name", StringType{}, Repetition::kOptional);
  EXPECT_EQ(f2.name, "name");
  EXPECT_TRUE(std::holds_alternative<StringType>(f2.data_type));
  EXPECT_EQ(f2.repetition, Repetition::kOptional);
  EXPECT_TRUE(f2.IsNullable());
}

TEST(FieldTest, ToString) {
  Field f("age", Int32Type{}, Repetition::kOptional);
  std::string str = f.ToString();
  EXPECT_NE(str.find("age"), std::string::npos);
  EXPECT_NE(str.find("Int32"), std::string::npos);
  EXPECT_NE(str.find("optional"), std::string::npos);
}

// =============================================================================
// ColumnStatistics Tests
// =============================================================================

TEST(ColumnStatisticsTest, HasMinMax) {
  ColumnStatistics stats1;
  EXPECT_FALSE(stats1.HasMinMax());

  ColumnStatistics stats2;
  stats2.min_value = std::vector<uint8_t>{0, 0, 0, 1};
  EXPECT_FALSE(stats2.HasMinMax());

  ColumnStatistics stats3;
  stats3.min_value = std::vector<uint8_t>{0, 0, 0, 1};
  stats3.max_value = std::vector<uint8_t>{0, 0, 0, 100};
  EXPECT_TRUE(stats3.HasMinMax());
}

// =============================================================================
// Schema Tests
// =============================================================================

TEST(SchemaTest, Construction) {
  Schema empty_schema;
  EXPECT_EQ(empty_schema.num_fields(), 0);

  std::vector<Field> fields = {
      Field("id", Int64Type{}),
      Field("name", StringType{}, Repetition::kOptional),
      Field("score", DoubleType{})};
  Schema schema(fields);
  EXPECT_EQ(schema.num_fields(), 3);
}

TEST(SchemaTest, FieldAccess) {
  std::vector<Field> fields = {
      Field("id", Int64Type{}),
      Field("name", StringType{}),
      Field("score", DoubleType{})};
  Schema schema(fields);

  EXPECT_EQ(schema.field(0).name, "id");
  EXPECT_EQ(schema.field(1).name, "name");
  EXPECT_EQ(schema.field(2).name, "score");
}

TEST(SchemaTest, GetFieldIndex) {
  std::vector<Field> fields = {
      Field("id", Int64Type{}),
      Field("name", StringType{}),
      Field("score", DoubleType{})};
  Schema schema(fields);

  auto id_idx = schema.GetFieldIndex("id");
  ASSERT_TRUE(id_idx.has_value());
  EXPECT_EQ(*id_idx, 0);

  auto name_idx = schema.GetFieldIndex("name");
  ASSERT_TRUE(name_idx.has_value());
  EXPECT_EQ(*name_idx, 1);

  auto missing_idx = schema.GetFieldIndex("nonexistent");
  EXPECT_FALSE(missing_idx.has_value());
}

TEST(SchemaTest, GetFieldByName) {
  std::vector<Field> fields = {
      Field("id", Int64Type{}),
      Field("name", StringType{})};
  Schema schema(fields);

  const Field* id_field = schema.GetFieldByName("id");
  ASSERT_NE(id_field, nullptr);
  EXPECT_EQ(id_field->name, "id");

  const Field* missing = schema.GetFieldByName("nonexistent");
  EXPECT_EQ(missing, nullptr);
}

TEST(SchemaTest, Project) {
  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", Int64Type{}),
      Field("c", DoubleType{}),
      Field("d", StringType{})};
  Schema schema(fields);

  ASSERT_OK_AND_ASSIGN(Schema projected, schema.Project({1, 3}));
  EXPECT_EQ(projected.num_fields(), 2);
  EXPECT_EQ(projected.field(0).name, "b");
  EXPECT_EQ(projected.field(1).name, "d");

  auto out_of_range = schema.Project({5});
  ASSERT_FALSE(out_of_range.has_value());
  EXPECT_EQ(out_of_range.error().code(), quest::ErrorCode::OutOfRange);
}

TEST(SchemaTest, Select) {
  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", Int64Type{}),
      Field("c", DoubleType{})};
  Schema schema(fields);

  ASSERT_OK_AND_ASSIGN(Schema selected, schema.Select({"c", "a"}));
  EXPECT_EQ(selected.num_fields(), 2);
  EXPECT_EQ(selected.field(0).name, "c");
  EXPECT_EQ(selected.field(1).name, "a");

  auto missing = schema.Select({"nonexistent"});
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), quest::ErrorCode::SchemaError);
}

TEST(SchemaTest, ToString) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{})};
  Schema schema(fields);

  std::string str = schema.ToString();
  EXPECT_NE(str.find("Schema"), std::string::npos);
  EXPECT_NE(str.find("id"), std::string::npos);
  EXPECT_NE(str.find("name"), std::string::npos);
}

// =============================================================================
// TypedColumnVector Tests
// =============================================================================

TEST(TypedColumnVectorTest, Int32Basic) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  EXPECT_EQ(col.size(), 0);

  col.Append(100);
  col.Append(200);
  col.Append(300);

  EXPECT_EQ(col.size(), 3);
  EXPECT_EQ(col.GetTypedValue(0), 100);
  EXPECT_EQ(col.GetTypedValue(1), 200);
  EXPECT_EQ(col.GetTypedValue(2), 300);

  EXPECT_FALSE(col.IsNull(0));
  EXPECT_FALSE(col.IsNull(1));
  EXPECT_FALSE(col.IsNull(2));
}

TEST(TypedColumnVectorTest, Nulls) {
  TypedColumnVector<int64_t> col(Int64Type{}, 10);

  col.Append(1);
  col.AppendNull();
  col.Append(3);

  EXPECT_EQ(col.size(), 3);
  EXPECT_FALSE(col.IsNull(0));
  EXPECT_TRUE(col.IsNull(1));
  EXPECT_FALSE(col.IsNull(2));

  Value val = col.GetValue(1);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

  val = col.GetValue(0);
  ASSERT_TRUE(std::holds_alternative<int64_t>(val));
  EXPECT_EQ(std::get<int64_t>(val), 1);
}

TEST(TypedColumnVectorTest, SetValue) {
  TypedColumnVector<double> col(DoubleType{}, 5);
  col.Append(1.0);
  col.Append(2.0);

  col.SetValue(0, 10.0);
  EXPECT_EQ(col.GetTypedValue(0), 10.0);
  EXPECT_FALSE(col.IsNull(0));

  col.SetNull(0);
  EXPECT_TRUE(col.IsNull(0));

  col.SetValid(0);
  EXPECT_FALSE(col.IsNull(0));
}

TEST(TypedColumnVectorTest, Bool) {
  TypedColumnVector<bool> col(BoolType{}, 5);
  col.Append(true);
  col.Append(false);
  col.AppendNull();
  col.Append(true);

  EXPECT_EQ(col.size(), 4);
  EXPECT_EQ(col.GetTypedValue(0), true);
  EXPECT_EQ(col.GetTypedValue(1), false);
  EXPECT_TRUE(col.IsNull(2));
  EXPECT_EQ(col.GetTypedValue(3), true);
}

TEST(TypedColumnVectorTest, Float) {
  TypedColumnVector<float> col(FloatType{}, 5);
  col.Append(1.5f);
  col.Append(2.5f);

  EXPECT_EQ(col.size(), 2);
  EXPECT_FLOAT_EQ(col.GetTypedValue(0), 1.5f);
  EXPECT_FLOAT_EQ(col.GetTypedValue(1), 2.5f);
}

TEST(TypedColumnVectorTest, Resize) {
  TypedColumnVector<int32_t> col(Int32Type{}, 2);

  for (int i = 0; i < 100; ++i) {
    col.Append(i);
  }

  EXPECT_EQ(col.size(), 100);
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(col.GetTypedValue(i), i);
    EXPECT_FALSE(col.IsNull(i));
  }
}

TEST(TypedColumnVectorTest, Spans) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  col.Append(1);
  col.Append(2);
  col.Append(3);

  auto values = col.GetValues();
  EXPECT_EQ(values.size(), 3);
  EXPECT_EQ(values[0], 1);
  EXPECT_EQ(values[1], 2);
  EXPECT_EQ(values[2], 3);

  auto mutable_values = col.GetMutableValues();
  mutable_values[0] = 100;
  EXPECT_EQ(col.GetTypedValue(0), 100);
}

// =============================================================================
// StringColumnVector Tests
// =============================================================================

TEST(StringColumnVectorTest, Basic) {
  StringColumnVector col(StringType{}, 10);
  EXPECT_EQ(col.size(), 0);

  col.Append("hello");
  col.Append("world");

  EXPECT_EQ(col.size(), 2);
  EXPECT_EQ(col.GetString(0), "hello");
  EXPECT_EQ(col.GetString(1), "world");
  EXPECT_FALSE(col.IsNull(0));
  EXPECT_FALSE(col.IsNull(1));
}

TEST(StringColumnVectorTest, Nulls) {
  StringColumnVector col(StringType{});

  col.Append("first");
  col.AppendNull();
  col.Append("third");

  EXPECT_EQ(col.size(), 3);
  EXPECT_FALSE(col.IsNull(0));
  EXPECT_TRUE(col.IsNull(1));
  EXPECT_FALSE(col.IsNull(2));

  Value val = col.GetValue(1);
  EXPECT_TRUE(std::holds_alternative<std::monostate>(val));

  val = col.GetValue(0);
  ASSERT_TRUE(std::holds_alternative<std::string>(val));
  EXPECT_EQ(std::get<std::string>(val), "first");
}

TEST(StringColumnVectorTest, Binary) {
  StringColumnVector col(BinaryType{});
  col.Append(std::string("\x00\x01\x02", 3));
  col.Append(std::string("\xFF\xFE", 2));

  EXPECT_EQ(col.size(), 2);
  EXPECT_EQ(col.GetString(0).size(), 3);
  EXPECT_EQ(col.GetString(1).size(), 2);
}

TEST(StringColumnVectorTest, Resize) {
  StringColumnVector col(StringType{}, 2);

  for (int i = 0; i < 100; ++i) {
    col.Append("value_" + std::to_string(i));
  }

  EXPECT_EQ(col.size(), 100);
  EXPECT_EQ(col.GetString(0), "value_0");
  EXPECT_EQ(col.GetString(99), "value_99");
}

// =============================================================================
// CreateColumnVector Factory Tests
// =============================================================================

TEST(CreateColumnVectorTest, Bool) {
  auto col = CreateColumnVector(BoolType{}, 10);
  EXPECT_NE(dynamic_cast<TypedColumnVector<bool>*>(col.get()), nullptr);
}

TEST(CreateColumnVectorTest, Int32) {
  auto col = CreateColumnVector(Int32Type{}, 10);
  EXPECT_NE(dynamic_cast<TypedColumnVector<int32_t>*>(col.get()), nullptr);
}

TEST(CreateColumnVectorTest, Int64) {
  auto col = CreateColumnVector(Int64Type{}, 10);
  EXPECT_NE(dynamic_cast<TypedColumnVector<int64_t>*>(col.get()), nullptr);
}

TEST(CreateColumnVectorTest, Float) {
  auto col = CreateColumnVector(FloatType{}, 10);
  EXPECT_NE(dynamic_cast<TypedColumnVector<float>*>(col.get()), nullptr);
}

TEST(CreateColumnVectorTest, Double) {
  auto col = CreateColumnVector(DoubleType{}, 10);
  EXPECT_NE(dynamic_cast<TypedColumnVector<double>*>(col.get()), nullptr);
}

TEST(CreateColumnVectorTest, String) {
  auto col = CreateColumnVector(StringType{}, 10);
  EXPECT_NE(dynamic_cast<StringColumnVector*>(col.get()), nullptr);
}

TEST(CreateColumnVectorTest, Binary) {
  auto col = CreateColumnVector(BinaryType{}, 10);
  EXPECT_NE(dynamic_cast<StringColumnVector*>(col.get()), nullptr);
}

// =============================================================================
// Value Tests
// =============================================================================

TEST(ValueTest, ToStringNull) {
  Value null_val = std::monostate{};
  EXPECT_EQ(ValueToString(null_val), "null");
}

TEST(ValueTest, ToStringBool) {
  Value true_val = true;
  Value false_val = false;
  EXPECT_EQ(ValueToString(true_val), "true");
  EXPECT_EQ(ValueToString(false_val), "false");
}

TEST(ValueTest, ToStringNumbers) {
  Value int_val = int32_t(42);
  EXPECT_EQ(ValueToString(int_val), "42");

  Value long_val = int64_t(-12345);
  EXPECT_EQ(ValueToString(long_val), "-12345");

  Value double_val = 3.14;
  std::string double_str = ValueToString(double_val);
  EXPECT_NE(double_str.find("3.14"), std::string::npos);
}

TEST(ValueTest, ToStringString) {
  Value str_val = std::string("hello");
  EXPECT_EQ(ValueToString(str_val), "hello");
}

// =============================================================================
// RecordBatch Tests
// =============================================================================

TEST(RecordBatchTest, Construction) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{})};
  Schema schema(fields);

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 10);
  id_col->Append(1);
  id_col->Append(2);

  auto name_col = std::make_shared<StringColumnVector>(StringType{});
  name_col->Append("Alice");
  name_col->Append("Bob");

  RecordBatch batch(schema, {id_col, name_col});

  EXPECT_EQ(batch.num_rows(), 2);
  EXPECT_EQ(batch.num_columns(), 2);
}

TEST(RecordBatchTest, ColumnAccess) {
  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", Int64Type{})};
  Schema schema(fields);

  auto col_a = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  col_a->Append(10);
  col_a->Append(20);

  auto col_b = std::make_shared<TypedColumnVector<int64_t>>(Int64Type{}, 5);
  col_b->Append(100);
  col_b->Append(200);

  RecordBatch batch(schema, {col_a, col_b});

  const ColumnVector& col0 = batch.column(0);
  EXPECT_EQ(col0.size(), 2);

  auto col_ptr = batch.column_ptr(1);
  EXPECT_EQ(col_ptr->size(), 2);
}

TEST(RecordBatchTest, ColumnByName) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("value", DoubleType{})};
  Schema schema(fields);

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  id_col->Append(1);

  auto val_col = std::make_shared<TypedColumnVector<double>>(DoubleType{}, 5);
  val_col->Append(3.14);

  RecordBatch batch(schema, {id_col, val_col});

  const ColumnVector* id = batch.GetColumnByName("id");
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(id->size(), 1);

  const ColumnVector* missing = batch.GetColumnByName("nonexistent");
  EXPECT_EQ(missing, nullptr);
}

TEST(RecordBatchTest, FieldAlias) {
  std::vector<Field> fields = {Field("a", Int32Type{})};
  Schema schema(fields);

  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  col->Append(42);

  RecordBatch batch(schema, {col});

  const ColumnVector& via_field = batch.field(0);
  const ColumnVector& via_column = batch.column(0);

  EXPECT_EQ(&via_field, &via_column);
}

TEST(RecordBatchTest, Empty) {
  Schema schema(std::vector<Field>{});
  RecordBatch batch(schema, {});

  EXPECT_EQ(batch.num_rows(), 0);
  EXPECT_EQ(batch.num_columns(), 0);
}

TEST(RecordBatchTest, ToCSV) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{})};
  Schema schema(fields);

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  id_col->Append(1);
  id_col->Append(2);

  auto name_col = std::make_shared<StringColumnVector>(StringType{});
  name_col->Append("Alice");
  name_col->Append("Bob");

  RecordBatch batch(schema, {id_col, name_col});

  std::string csv = batch.ToCSV();
  EXPECT_NE(csv.find("1,Alice"), std::string::npos);
  EXPECT_NE(csv.find("2,Bob"), std::string::npos);
}

TEST(RecordBatchTest, ToCSVWithNulls) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{}, Repetition::kOptional)};
  Schema schema(fields);

  auto id_col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, 5);
  id_col->Append(1);
  id_col->Append(2);

  auto name_col = std::make_shared<StringColumnVector>(StringType{});
  name_col->Append("Alice");
  name_col->AppendNull();

  RecordBatch batch(schema, {id_col, name_col});

  std::string csv = batch.ToCSV();
  EXPECT_NE(csv.find("1,Alice"), std::string::npos);
  EXPECT_NE(csv.find("2,null"), std::string::npos);
}
