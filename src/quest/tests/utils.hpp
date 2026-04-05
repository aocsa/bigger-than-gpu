#pragma once
#include <gtest/gtest.h>
#include <utility> // for std::move

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/datasource/types.hpp"

// Helper macro to generate unique variable names
#define _QUEST_GTEST_CONCAT_IMPL(x, y) x##y
#define _QUEST_GTEST_CONCAT(x, y) _QUEST_GTEST_CONCAT_IMPL(x, y)
#define _QUEST_GTEST_NAME(x) _QUEST_GTEST_CONCAT(x, __LINE__)

namespace quest::testing {

/// @brief Creates a simple RecordBatch with a single int32 column for testing
/// @param num_rows Number of rows to generate (values will be 0, 1, 2, ...)
/// @return A RecordBatch with one column named "value"
inline datasource::RecordBatch MakeTestRecordBatch(size_t num_rows) {
  using namespace quest::datasource;

  std::vector<Field> fields = {Field("value", Int32Type{})};
  Schema schema(fields);

  auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, num_rows);
  for (size_t i = 0; i < num_rows; ++i) {
    col->Append(static_cast<int32_t>(i));
  }

  return RecordBatch(schema, {col});
}

/// @brief Creates a RecordBatch with multiple columns for testing
/// @param num_rows Number of rows
/// @param num_columns Number of int32 columns to create
/// @return A RecordBatch with columns named "col0", "col1", etc.
inline datasource::RecordBatch MakeTestRecordBatch(size_t num_rows,
                                                    size_t num_columns) {
  using namespace quest::datasource;

  std::vector<Field> fields;
  std::vector<std::shared_ptr<ColumnVector>> columns;

  for (size_t c = 0; c < num_columns; ++c) {
    fields.emplace_back("col" + std::to_string(c), Int32Type{});
    auto col = std::make_shared<TypedColumnVector<int32_t>>(Int32Type{}, num_rows);
    for (size_t r = 0; r < num_rows; ++r) {
      col->Append(static_cast<int32_t>(r * num_columns + c));
    }
    columns.push_back(col);
  }

  Schema schema(fields);
  return RecordBatch(schema, columns);
}

}  // namespace quest::testing

// ---------------------------------------------------------
// ASSERT_OK
// Checks if a Status or Result is OK. Fails the test if not.
// ---------------------------------------------------------
#define ASSERT_OK(expr)                                                    \
  do {                                                                     \
    auto _res = (expr);                                                    \
    ASSERT_TRUE(_res.has_value())                                          \
        << "Operation failed: " << #expr << "\n"                           \
        << "Error: " << _res.error().ToString();                           \
  } while (0)

// ---------------------------------------------------------
// ASSERT_OK_AND_ASSIGN
// 1. Executes the expression (returning Result<T>).
// 2. Asserts that it has a value.
// 3. If successful, moves the inner value into 'lhs'.
//
// Usage: ASSERT_OK_AND_ASSIGN(auto value, MyFunc());
// ---------------------------------------------------------
#define ASSERT_OK_AND_ASSIGN(lhs, expr)                                    \
  /* 1. Create a unique variable to hold the result */                     \
  auto _QUEST_GTEST_NAME(_result) = (expr);                                \
                                                                           \
  /* 2. Verify success using GTest's ASSERT_TRUE */                        \
  ASSERT_TRUE(_QUEST_GTEST_NAME(_result).has_value())                      \
      << "Operation failed: " << #expr << "\n"                             \
      << "Error: " << _QUEST_GTEST_NAME(_result).error().ToString();       \
                                                                           \
  /* 3. Assign/Move the value to the user's variable */                    \
  lhs = std::move(*_QUEST_GTEST_NAME(_result));