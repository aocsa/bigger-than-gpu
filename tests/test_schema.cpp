// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_schema.cpp
/// @brief Unit tests for Schema and Field classes.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "quest/datasource/schema.hpp"
#include "quest/tests/utils.hpp"

using namespace quest::datasource;

// =============================================================================
// Field Tests
// =============================================================================

TEST(FieldTest, Construction) {
  Field f("id", Int32Type{});
  EXPECT_EQ(f.name, "id");
  EXPECT_TRUE(std::holds_alternative<Int32Type>(f.data_type));
  EXPECT_EQ(f.repetition, Repetition::kRequired);
}

TEST(FieldTest, NullableField) {
  Field f("nullable_col", StringType{}, Repetition::kOptional);
  EXPECT_TRUE(f.IsNullable());

  Field f2("required_col", Int32Type{}, Repetition::kRequired);
  EXPECT_FALSE(f2.IsNullable());
}

TEST(FieldTest, ToString) {
  Field f("age", Int32Type{});
  std::string str = f.ToString();
  EXPECT_FALSE(str.empty());
  EXPECT_NE(str.find("age"), std::string::npos);
}

TEST(FieldTest, AllDataTypes) {
  EXPECT_NO_THROW(Field("bool_field", BoolType{}));
  EXPECT_NO_THROW(Field("int8_field", Int8Type{}));
  EXPECT_NO_THROW(Field("int16_field", Int16Type{}));
  EXPECT_NO_THROW(Field("int32_field", Int32Type{}));
  EXPECT_NO_THROW(Field("int64_field", Int64Type{}));
  EXPECT_NO_THROW(Field("float_field", FloatType{}));
  EXPECT_NO_THROW(Field("double_field", DoubleType{}));
  EXPECT_NO_THROW(Field("string_field", StringType{}));
  EXPECT_NO_THROW(Field("binary_field", BinaryType{}));
}

TEST(FieldTest, WithFieldId) {
  Field f("col", Int32Type{});
  f.field_id = 42;
  EXPECT_TRUE(f.field_id.has_value());
  EXPECT_EQ(*f.field_id, 42);
}

// =============================================================================
// Schema Tests
// =============================================================================

TEST(SchemaTest, EmptySchema) {
  Schema s;
  EXPECT_EQ(s.num_fields(), 0U);
  EXPECT_TRUE(s.fields().empty());
}

TEST(SchemaTest, Construction) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{}),
      Field("salary", DoubleType{}),
  };

  Schema s(fields);

  EXPECT_EQ(s.num_fields(), 3U);
  EXPECT_EQ(s.field(0).name, "id");
  EXPECT_EQ(s.field(1).name, "name");
  EXPECT_EQ(s.field(2).name, "salary");
}

TEST(SchemaTest, FieldAccess) {
  std::vector<Field> fields = {
      Field("col1", Int32Type{}),
      Field("col2", StringType{}),
  };
  Schema s(fields);

  EXPECT_EQ(s.field(0).name, "col1");
  EXPECT_EQ(s.field(1).name, "col2");
  EXPECT_THROW(s.field(2), std::out_of_range);
}

TEST(SchemaTest, GetFieldIndex) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{}),
      Field("value", DoubleType{}),
  };
  Schema s(fields);

  auto idx_id = s.GetFieldIndex("id");
  ASSERT_TRUE(idx_id.has_value());
  EXPECT_EQ(*idx_id, 0U);

  auto idx_name = s.GetFieldIndex("name");
  ASSERT_TRUE(idx_name.has_value());
  EXPECT_EQ(*idx_name, 1U);

  auto idx_value = s.GetFieldIndex("value");
  ASSERT_TRUE(idx_value.has_value());
  EXPECT_EQ(*idx_value, 2U);

  auto idx_nonexistent = s.GetFieldIndex("nonexistent");
  EXPECT_FALSE(idx_nonexistent.has_value());
}

TEST(SchemaTest, GetFieldByName) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{}),
  };
  Schema s(fields);

  const Field* f = s.GetFieldByName("id");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->name, "id");

  f = s.GetFieldByName("name");
  ASSERT_NE(f, nullptr);
  EXPECT_EQ(f->name, "name");

  f = s.GetFieldByName("nonexistent");
  EXPECT_EQ(f, nullptr);
}

TEST(SchemaTest, Project) {
  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", StringType{}),
      Field("c", DoubleType{}),
      Field("d", BoolType{}),
  };
  Schema s(fields);

  Schema projected = s.Project({0, 2});

  EXPECT_EQ(projected.num_fields(), 2U);
  EXPECT_EQ(projected.field(0).name, "a");
  EXPECT_EQ(projected.field(1).name, "c");
}

TEST(SchemaTest, ProjectReorder) {
  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", StringType{}),
      Field("c", DoubleType{}),
  };
  Schema s(fields);

  Schema projected = s.Project({2, 1, 0});

  EXPECT_EQ(projected.num_fields(), 3U);
  EXPECT_EQ(projected.field(0).name, "c");
  EXPECT_EQ(projected.field(1).name, "b");
  EXPECT_EQ(projected.field(2).name, "a");
}

TEST(SchemaTest, Select) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{}),
      Field("email", StringType{}),
      Field("age", Int32Type{}),
  };
  Schema s(fields);

  Schema selected = s.Select({"name", "age"});

  EXPECT_EQ(selected.num_fields(), 2U);
  EXPECT_EQ(selected.field(0).name, "name");
  EXPECT_EQ(selected.field(1).name, "age");
}

TEST(SchemaTest, SelectNonexistent) {
  std::vector<Field> fields = {
      Field("a", Int32Type{}),
      Field("b", StringType{}),
  };
  Schema s(fields);

  EXPECT_THROW(s.Select({"a", "nonexistent", "b"}), std::invalid_argument);
}

TEST(SchemaTest, ToString) {
  std::vector<Field> fields = {
      Field("id", Int32Type{}),
      Field("name", StringType{}),
  };
  Schema s(fields);

  std::string str = s.ToString();
  EXPECT_FALSE(str.empty());
  EXPECT_NE(str.find("id"), std::string::npos);
  EXPECT_NE(str.find("name"), std::string::npos);
}

// =============================================================================
// ColumnStatistics Tests
// =============================================================================

TEST(ColumnStatisticsTest, HasMinMax) {
  ColumnStatistics stats;
  EXPECT_FALSE(stats.HasMinMax());

  stats.min_value = std::vector<uint8_t>{1, 2, 3};
  EXPECT_FALSE(stats.HasMinMax());

  stats.max_value = std::vector<uint8_t>{4, 5, 6};
  EXPECT_TRUE(stats.HasMinMax());
}

TEST(ColumnStatisticsTest, OptionalFields) {
  ColumnStatistics stats;

  EXPECT_FALSE(stats.null_count.has_value());
  EXPECT_FALSE(stats.distinct_count.has_value());
  EXPECT_FALSE(stats.num_values.has_value());

  stats.null_count = 10;
  stats.distinct_count = 100;
  stats.num_values = 1000;

  EXPECT_EQ(*stats.null_count, 10);
  EXPECT_EQ(*stats.distinct_count, 100);
  EXPECT_EQ(*stats.num_values, 1000);
}

// =============================================================================
// Type Helper Tests
// =============================================================================

TEST(TypeHelpersTest, GetTypeName) {
  EXPECT_EQ(GetTypeName(BoolType{}), "Bool");
  EXPECT_EQ(GetTypeName(Int32Type{}), "Int32");
  EXPECT_EQ(GetTypeName(Int64Type{}), "Int64");
  EXPECT_EQ(GetTypeName(FloatType{}), "Float");
  EXPECT_EQ(GetTypeName(DoubleType{}), "Double");
  EXPECT_EQ(GetTypeName(StringType{}), "String");
  EXPECT_EQ(GetTypeName(BinaryType{}), "Binary");
}

TEST(TypeHelpersTest, GetTypeByteWidth) {
  EXPECT_EQ(GetTypeByteWidth(BoolType{}), 1);
  EXPECT_EQ(GetTypeByteWidth(Int8Type{}), 1);
  EXPECT_EQ(GetTypeByteWidth(Int16Type{}), 2);
  EXPECT_EQ(GetTypeByteWidth(Int32Type{}), 4);
  EXPECT_EQ(GetTypeByteWidth(Int64Type{}), 8);
  EXPECT_EQ(GetTypeByteWidth(FloatType{}), 4);
  EXPECT_EQ(GetTypeByteWidth(DoubleType{}), 8);
}

TEST(TypeHelpersTest, IsFixedWidth) {
  EXPECT_TRUE(IsFixedWidth(BoolType{}));
  EXPECT_TRUE(IsFixedWidth(Int32Type{}));
  EXPECT_TRUE(IsFixedWidth(DoubleType{}));
  EXPECT_FALSE(IsFixedWidth(StringType{}));
  EXPECT_FALSE(IsFixedWidth(BinaryType{}));
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(SchemaEdgeCaseTest, DuplicateFieldNames) {
  // Schema should allow duplicate field names (though not recommended)
  std::vector<Field> fields = {
      Field("col", Int32Type{}),
      Field("col", StringType{}),
  };
  Schema s(fields);

  EXPECT_EQ(s.num_fields(), 2U);
  // GetFieldIndex returns first match
  auto idx = s.GetFieldIndex("col");
  EXPECT_EQ(*idx, 0U);
}

TEST(SchemaEdgeCaseTest, EmptyFieldName) {
  Field f("", Int32Type{});
  EXPECT_TRUE(f.name.empty());
}

TEST(SchemaEdgeCaseTest, LargeSchema) {
  std::vector<Field> fields;
  for (int i = 0; i < 1000; ++i) {
    fields.emplace_back("col_" + std::to_string(i), Int32Type{});
  }
  Schema s(fields);

  EXPECT_EQ(s.num_fields(), 1000U);
  EXPECT_EQ(s.field(999).name, "col_999");
}
