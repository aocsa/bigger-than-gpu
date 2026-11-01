// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_schema.cpp
/// @brief Unit tests for Schema and Field classes.

#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "quest/columnar/schema.hpp"
#include "support/test_utils.hpp"

using namespace quest::columnar;

// =============================================================================
// Field Tests
// =============================================================================

TEST(FieldTest, Construction) {
  Field f("id", DataType::kInt32);
  EXPECT_EQ(f.name, "id");
  EXPECT_TRUE(f.data_type == DataType::kInt32);
  EXPECT_EQ(f.repetition, Repetition::kRequired);
}

TEST(FieldTest, NullableField) {
  Field f("nullable_col", DataType::kString, Repetition::kOptional);
  EXPECT_TRUE(f.IsNullable());

  Field f2("required_col", DataType::kInt32, Repetition::kRequired);
  EXPECT_FALSE(f2.IsNullable());
}

TEST(FieldTest, ToString) {
  Field f("age", DataType::kInt32);
  std::string str = f.ToString();
  EXPECT_FALSE(str.empty());
  EXPECT_NE(str.find("age"), std::string::npos);
}

TEST(FieldTest, AllDataTypes) {
  EXPECT_NO_THROW(Field("bool_field", DataType::kBool));
  EXPECT_NO_THROW(Field("int8_field", DataType::kInt8));
  EXPECT_NO_THROW(Field("int16_field", DataType::kInt16));
  EXPECT_NO_THROW(Field("int32_field", DataType::kInt32));
  EXPECT_NO_THROW(Field("int64_field", DataType::kInt64));
  EXPECT_NO_THROW(Field("float_field", DataType::kFloat));
  EXPECT_NO_THROW(Field("double_field", DataType::kDouble));
  EXPECT_NO_THROW(Field("string_field", DataType::kString));
  EXPECT_NO_THROW(Field("binary_field", DataType::kString));
}

TEST(FieldTest, WithFieldId) {
  Field f("col", DataType::kInt32);
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
      Field("id", DataType::kInt32),
      Field("name", DataType::kString),
      Field("salary", DataType::kDouble),
  };

  Schema s(fields);

  EXPECT_EQ(s.num_fields(), 3U);
  EXPECT_EQ(s.field(0).name, "id");
  EXPECT_EQ(s.field(1).name, "name");
  EXPECT_EQ(s.field(2).name, "salary");
}

TEST(SchemaTest, FieldAccess) {
  std::vector<Field> fields = {
      Field("col1", DataType::kInt32),
      Field("col2", DataType::kString),
  };
  Schema s(fields);

  EXPECT_EQ(s.field(0).name, "col1");
  EXPECT_EQ(s.field(1).name, "col2");
}

TEST(SchemaTest, GetFieldIndex) {
  std::vector<Field> fields = {
      Field("id", DataType::kInt32),
      Field("name", DataType::kString),
      Field("value", DataType::kDouble),
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
      Field("id", DataType::kInt32),
      Field("name", DataType::kString),
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
      Field("a", DataType::kInt32),
      Field("b", DataType::kString),
      Field("c", DataType::kDouble),
      Field("d", DataType::kBool),
  };
  Schema s(fields);

  ASSERT_OK_AND_ASSIGN(Schema projected, s.Project({0, 2}));

  EXPECT_EQ(projected.num_fields(), 2U);
  EXPECT_EQ(projected.field(0).name, "a");
  EXPECT_EQ(projected.field(1).name, "c");
}

TEST(SchemaTest, ProjectReorder) {
  std::vector<Field> fields = {
      Field("a", DataType::kInt32),
      Field("b", DataType::kString),
      Field("c", DataType::kDouble),
  };
  Schema s(fields);

  ASSERT_OK_AND_ASSIGN(Schema projected, s.Project({2, 1, 0}));

  EXPECT_EQ(projected.num_fields(), 3U);
  EXPECT_EQ(projected.field(0).name, "c");
  EXPECT_EQ(projected.field(1).name, "b");
  EXPECT_EQ(projected.field(2).name, "a");
}

TEST(SchemaTest, Select) {
  std::vector<Field> fields = {
      Field("id", DataType::kInt32),
      Field("name", DataType::kString),
      Field("email", DataType::kString),
      Field("age", DataType::kInt32),
  };
  Schema s(fields);

  ASSERT_OK_AND_ASSIGN(Schema selected, s.Select({"name", "age"}));

  EXPECT_EQ(selected.num_fields(), 2U);
  EXPECT_EQ(selected.field(0).name, "name");
  EXPECT_EQ(selected.field(1).name, "age");
}

TEST(SchemaTest, SelectNonexistent) {
  std::vector<Field> fields = {
      Field("a", DataType::kInt32),
      Field("b", DataType::kString),
  };
  Schema s(fields);

  auto selected = s.Select({"a", "nonexistent", "b"});
  ASSERT_FALSE(selected.has_value());
  EXPECT_EQ(selected.error().code(), quest::ErrorCode::SchemaError);
}

TEST(SchemaTest, ToString) {
  std::vector<Field> fields = {
      Field("id", DataType::kInt32),
      Field("name", DataType::kString),
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
  EXPECT_EQ(GetTypeName(DataType::kBool), "Bool");
  EXPECT_EQ(GetTypeName(DataType::kInt32), "Int32");
  EXPECT_EQ(GetTypeName(DataType::kInt64), "Int64");
  EXPECT_EQ(GetTypeName(DataType::kFloat), "Float");
  EXPECT_EQ(GetTypeName(DataType::kDouble), "Double");
  EXPECT_EQ(GetTypeName(DataType::kString), "String");
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(SchemaEdgeCaseTest, DuplicateFieldNames) {
  // Schema should allow duplicate field names (though not recommended)
  std::vector<Field> fields = {
      Field("col", DataType::kInt32),
      Field("col", DataType::kString),
  };
  Schema s(fields);

  EXPECT_EQ(s.num_fields(), 2U);
  // GetFieldIndex returns first match
  auto idx = s.GetFieldIndex("col");
  EXPECT_EQ(*idx, 0U);
}

TEST(SchemaEdgeCaseTest, EmptyFieldName) {
  Field f("", DataType::kInt32);
  EXPECT_TRUE(f.name.empty());
}

TEST(SchemaEdgeCaseTest, LargeSchema) {
  std::vector<Field> fields;
  for (int i = 0; i < 1000; ++i) {
    fields.emplace_back("col_" + std::to_string(i), DataType::kInt32);
  }
  Schema s(fields);

  EXPECT_EQ(s.num_fields(), 1000U);
  EXPECT_EQ(s.field(999).name, "col_999");
}
