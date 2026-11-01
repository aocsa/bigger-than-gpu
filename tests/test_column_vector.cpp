// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_column_vector.cpp
/// @brief Unit tests for ColumnVector implementations.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "support/test_utils.hpp"

using namespace quest::columnar;

// =============================================================================
// TypedColumnVector<int32_t> Tests
// =============================================================================

TEST(TypedColumnVectorInt32Test, Construction) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  EXPECT_EQ(col.size(), 0U);
  EXPECT_TRUE(std::holds_alternative<Int32Type>(col.GetType()));
}

TEST(TypedColumnVectorInt32Test, AppendAndGet) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);

  col.Append(42);
  col.Append(100);
  col.Append(-5);

  EXPECT_EQ(col.size(), 3U);
  EXPECT_EQ(col.GetTypedValue(0), 42);
  EXPECT_EQ(col.GetTypedValue(1), 100);
  EXPECT_EQ(col.GetTypedValue(2), -5);
}

TEST(TypedColumnVectorInt32Test, GetValueVariant) {
  TypedColumnVector<int32_t> col(Int32Type{}, 5);
  col.Append(123);

  Value val = col.GetValue(0);
  ASSERT_TRUE(std::holds_alternative<int32_t>(val));
  EXPECT_EQ(std::get<int32_t>(val), 123);
}

TEST(TypedColumnVectorInt32Test, NullHandling) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);

  col.Append(10);
  col.AppendNull();
  col.Append(30);

  EXPECT_EQ(col.size(), 3U);
  EXPECT_FALSE(col.IsNull(0));
  EXPECT_TRUE(col.IsNull(1));
  EXPECT_FALSE(col.IsNull(2));
}

TEST(TypedColumnVectorInt32Test, SetValue) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  col.Append(0);
  col.Append(0);

  col.SetValue(0, 999);
  col.SetValue(1, -999);

  EXPECT_EQ(col.GetTypedValue(0), 999);
  EXPECT_EQ(col.GetTypedValue(1), -999);
}

TEST(TypedColumnVectorInt32Test, SetNull) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  col.Append(42);

  EXPECT_FALSE(col.IsNull(0));
  col.SetNull(0);
  EXPECT_TRUE(col.IsNull(0));
  col.SetValid(0);
  EXPECT_FALSE(col.IsNull(0));
}

TEST(TypedColumnVectorInt32Test, Resize) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  col.Append(1);
  col.Append(2);
  col.Append(3);

  col.SetSize(5);
  EXPECT_EQ(col.size(), 5U);

  col.SetSize(2);
  EXPECT_EQ(col.size(), 2U);
}

TEST(TypedColumnVectorInt32Test, Spans) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);
  col.Append(10);
  col.Append(20);
  col.Append(30);

  auto values = col.GetValues();
  EXPECT_EQ(values.size(), 3U);
  EXPECT_EQ(values[0], 10);
  EXPECT_EQ(values[1], 20);
  EXPECT_EQ(values[2], 30);

  auto mut_values = col.GetMutableValues();
  mut_values[0] = 100;
  EXPECT_EQ(col.GetTypedValue(0), 100);
}

// =============================================================================
// TypedColumnVector<int64_t> Tests
// =============================================================================

TEST(TypedColumnVectorInt64Test, BasicOperations) {
  TypedColumnVector<int64_t> col(Int64Type{}, 10);

  col.Append(1000000000000LL);
  col.Append(-1000000000000LL);

  EXPECT_EQ(col.size(), 2U);
  EXPECT_EQ(col.GetTypedValue(0), 1000000000000LL);
  EXPECT_EQ(col.GetTypedValue(1), -1000000000000LL);
}

// =============================================================================
// TypedColumnVector<double> Tests
// =============================================================================

TEST(TypedColumnVectorDoubleTest, BasicOperations) {
  TypedColumnVector<double> col(DoubleType{}, 10);

  col.Append(3.14159);
  col.Append(-2.71828);
  col.AppendNull();

  EXPECT_EQ(col.size(), 3U);
  EXPECT_NEAR(col.GetTypedValue(0), 3.14159, 1e-5);
  EXPECT_NEAR(col.GetTypedValue(1), -2.71828, 1e-5);
  EXPECT_TRUE(col.IsNull(2));
}

TEST(TypedColumnVectorDoubleTest, SpecialValues) {
  TypedColumnVector<double> col(DoubleType{}, 10);

  col.Append(std::numeric_limits<double>::infinity());
  col.Append(-std::numeric_limits<double>::infinity());
  col.Append(std::numeric_limits<double>::quiet_NaN());

  EXPECT_TRUE(std::isinf(col.GetTypedValue(0)));
  EXPECT_TRUE(std::isinf(col.GetTypedValue(1)));
  EXPECT_TRUE(std::isnan(col.GetTypedValue(2)));
}

// =============================================================================
// TypedColumnVector<float> Tests
// =============================================================================

TEST(TypedColumnVectorFloatTest, BasicOperations) {
  TypedColumnVector<float> col(FloatType{}, 10);

  col.Append(1.5f);
  col.Append(-2.5f);

  EXPECT_EQ(col.size(), 2U);
  EXPECT_NEAR(col.GetTypedValue(0), 1.5f, 1e-5f);
  EXPECT_NEAR(col.GetTypedValue(1), -2.5f, 1e-5f);
}

// =============================================================================
// TypedColumnVector<bool> Tests
// =============================================================================

TEST(TypedColumnVectorBoolTest, BasicOperations) {
  TypedColumnVector<bool> col(BoolType{}, 10);

  col.Append(true);
  col.Append(false);
  col.Append(true);

  EXPECT_EQ(col.size(), 3U);
  EXPECT_TRUE(col.GetTypedValue(0));
  EXPECT_FALSE(col.GetTypedValue(1));
  EXPECT_TRUE(col.GetTypedValue(2));
}

TEST(TypedColumnVectorBoolTest, NullHandling) {
  TypedColumnVector<bool> col(BoolType{}, 10);

  col.Append(true);
  col.AppendNull();
  col.Append(false);

  EXPECT_FALSE(col.IsNull(0));
  EXPECT_TRUE(col.IsNull(1));
  EXPECT_FALSE(col.IsNull(2));
}

// =============================================================================
// StringColumnVector Tests
// =============================================================================

TEST(StringColumnVectorTest, Construction) {
  StringColumnVector col(StringType{}, 10);
  EXPECT_EQ(col.size(), 0U);
  EXPECT_TRUE(std::holds_alternative<StringType>(col.GetType()));
}

TEST(StringColumnVectorTest, AppendAndGet) {
  StringColumnVector col(StringType{}, 10);

  col.Append("hello");
  col.Append("world");
  col.Append("");

  EXPECT_EQ(col.size(), 3U);
  EXPECT_EQ(col.GetString(0), "hello");
  EXPECT_EQ(col.GetString(1), "world");
  EXPECT_EQ(col.GetString(2), "");
}

TEST(StringColumnVectorTest, GetValueVariant) {
  StringColumnVector col(StringType{}, 5);
  col.Append("test");

  Value val = col.GetValue(0);
  ASSERT_TRUE(std::holds_alternative<std::string>(val));
  EXPECT_EQ(std::get<std::string>(val), "test");
}

TEST(StringColumnVectorTest, NullHandling) {
  StringColumnVector col(StringType{}, 10);

  col.Append("first");
  col.AppendNull();
  col.Append("third");

  EXPECT_FALSE(col.IsNull(0));
  EXPECT_TRUE(col.IsNull(1));
  EXPECT_FALSE(col.IsNull(2));
}

TEST(StringColumnVectorTest, UnicodeStrings) {
  StringColumnVector col(StringType{}, 10);

  col.Append("Hello, 世界!");
  col.Append("🎉🎊🎈");
  col.Append("Ñoño");

  EXPECT_EQ(col.GetString(0), "Hello, 世界!");
  EXPECT_EQ(col.GetString(1), "🎉🎊🎈");
  EXPECT_EQ(col.GetString(2), "Ñoño");
}

TEST(StringColumnVectorTest, LongStrings) {
  StringColumnVector col(StringType{}, 10);

  std::string long_str(10000, 'x');
  col.Append(long_str);

  EXPECT_EQ(col.GetString(0), long_str);
  EXPECT_EQ(col.GetString(0).size(), 10000U);
}

// =============================================================================
// Binary Column Vector Tests
// =============================================================================

TEST(StringColumnVectorTest, BinaryData) {
  StringColumnVector col(BinaryType{}, 10);

  // Build a binary string with embedded null bytes
  std::string binary_data;
  binary_data.push_back('\x01');
  binary_data.push_back('\x00');
  binary_data.push_back('\x02');
  binary_data.push_back('\x00');
  binary_data.push_back('\x03');
  col.Append(binary_data);

  EXPECT_EQ(col.GetString(0).size(), 5U);
}

// =============================================================================
// CreateColumnVector Factory Tests
// =============================================================================

TEST(CreateColumnVectorTest, Bool) {
  auto col = CreateColumnVector(BoolType{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<BoolType>(col->GetType()));
}

TEST(CreateColumnVectorTest, Int32) {
  auto col = CreateColumnVector(Int32Type{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<Int32Type>(col->GetType()));
}

TEST(CreateColumnVectorTest, Int64) {
  auto col = CreateColumnVector(Int64Type{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<Int64Type>(col->GetType()));
}

TEST(CreateColumnVectorTest, Float) {
  auto col = CreateColumnVector(FloatType{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<FloatType>(col->GetType()));
}

TEST(CreateColumnVectorTest, Double) {
  auto col = CreateColumnVector(DoubleType{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<DoubleType>(col->GetType()));
}

TEST(CreateColumnVectorTest, String) {
  auto col = CreateColumnVector(StringType{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<StringType>(col->GetType()));
}

TEST(CreateColumnVectorTest, Binary) {
  auto col = CreateColumnVector(BinaryType{}, 10);
  EXPECT_NE(col, nullptr);
  EXPECT_TRUE(std::holds_alternative<BinaryType>(col->GetType()));
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(ColumnVectorEdgeCaseTest, EmptyColumn) {
  TypedColumnVector<int32_t> col(Int32Type{}, 0);
  EXPECT_EQ(col.size(), 0U);
}

TEST(ColumnVectorEdgeCaseTest, LargeColumn) {
  TypedColumnVector<int32_t> col(Int32Type{}, 100000);

  for (int i = 0; i < 10000; ++i) {
    col.Append(i);
  }

  EXPECT_EQ(col.size(), 10000U);
  EXPECT_EQ(col.GetTypedValue(0), 0);
  EXPECT_EQ(col.GetTypedValue(9999), 9999);
}

TEST(ColumnVectorEdgeCaseTest, AllNulls) {
  TypedColumnVector<int32_t> col(Int32Type{}, 10);

  for (int i = 0; i < 5; ++i) {
    col.AppendNull();
  }

  EXPECT_EQ(col.size(), 5U);
  for (size_t i = 0; i < col.size(); ++i) {
    EXPECT_TRUE(col.IsNull(i));
  }
}

// =============================================================================
// Date32 Tests
// =============================================================================

TEST(Date32Test, ValueToStringIsIsoDate) {
  EXPECT_EQ(ValueToString(Value{Date32{9538}}), "1996-02-12");
  EXPECT_EQ(ValueToString(Value{Date32{0}}), "1970-01-01");
  EXPECT_EQ(ValueToString(Value{Date32{-1}}), "1969-12-31");
  EXPECT_EQ(ValueToString(Value{Date32{11016}}), "2000-02-29");
}

TEST(Date32Test, TypeTraits) {
  EXPECT_EQ(GetTypeName(Date32Type{}), "Date32");
  EXPECT_EQ(GetTypeByteWidth(Date32Type{}), 4);
  EXPECT_TRUE(IsFixedWidth(Date32Type{}));
}

TEST(Date32Test, CreateColumnVectorHoldsDate32Values) {
  auto col = CreateColumnVector(Date32Type{}, 2);
  auto* typed = dynamic_cast<TypedColumnVector<Date32>*>(col.get());
  ASSERT_NE(typed, nullptr);
  typed->Append(Date32{8766});
  typed->AppendNull();

  EXPECT_TRUE(std::holds_alternative<Date32Type>(col->GetType()));
  EXPECT_EQ(col->GetValue(0), Value{Date32{8766}});
  EXPECT_TRUE(col->IsNull(1));
}

TEST(Date32Test, ConcatenateKeepsDate32) {
  Schema schema({Field("d", Date32Type{}, Repetition::kOptional)});
  auto first = std::make_shared<TypedColumnVector<Date32>>(Date32Type{}, 1);
  first->Append(Date32{1});
  auto second = std::make_shared<TypedColumnVector<Date32>>(Date32Type{}, 2);
  second->AppendNull();
  second->Append(Date32{2});
  RecordBatch a(schema, {first});
  RecordBatch b(schema, {second});

  ASSERT_OK_AND_ASSIGN(auto merged, ConcatenateRecordBatches({&a, &b}));
  ASSERT_EQ(merged.num_rows(), 3U);
  EXPECT_TRUE(std::holds_alternative<Date32Type>(merged.column(0).GetType()));
  EXPECT_EQ(merged.column(0).GetValue(0), Value{Date32{1}});
  EXPECT_TRUE(merged.column(0).IsNull(1));
  EXPECT_EQ(merged.column(0).GetValue(2), Value{Date32{2}});
}
