#pragma once

#include <gtest/gtest.h>

#include <utility>

#include "quest/columnar/column_vector.hpp"
#include "quest/columnar/record_batch.hpp"
#include "quest/columnar/schema.hpp"
#include "quest/columnar/types.hpp"

// Helper macro to generate unique variable names
#define QUEST_TEST_CONCAT_IMPL(x, y) x##y
#define QUEST_TEST_CONCAT(x, y) QUEST_TEST_CONCAT_IMPL(x, y)
#define QUEST_TEST_NAME(x) QUEST_TEST_CONCAT(x, __LINE__)

namespace quest::testing {

/// @brief Single int32 column named "value"; values are 0, 1, 2, …
inline columnar::RecordBatch MakeTestRecordBatch(size_t num_rows) {
  using namespace quest::columnar;

  std::vector<Field> fields = {Field("value", DataType::kInt32)};
  Schema schema(fields);

  auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, num_rows);
  for (size_t i = 0; i < num_rows; ++i) {
    col->Append(static_cast<int32_t>(i));
  }

  return RecordBatch(schema, {col});
}

/// @brief Multiple int32 columns named "col0", "col1", etc.
inline columnar::RecordBatch MakeTestRecordBatch(size_t num_rows, size_t num_columns) {
  using namespace quest::columnar;

  std::vector<Field> fields;
  std::vector<std::shared_ptr<ColumnVector>> columns;

  for (size_t c = 0; c < num_columns; ++c) {
    fields.emplace_back("col" + std::to_string(c), DataType::kInt32);
    auto col = std::make_shared<TypedColumnVector<int32_t>>(DataType::kInt32, num_rows);
    for (size_t r = 0; r < num_rows; ++r) {
      col->Append(static_cast<int32_t>(r * num_columns + c));
    }
    columns.push_back(col);
  }

  Schema schema(fields);
  return RecordBatch(schema, columns);
}

}  // namespace quest::testing

// Fails the test if Status/Result is not OK.
#define ASSERT_OK(expr)                                        \
  do {                                                         \
    auto quest_test_status_ = (expr);                          \
    ASSERT_TRUE(quest_test_status_.has_value())                \
        << "Operation failed: " << #expr << "\n"               \
        << "Error: " << quest_test_status_.error().ToString(); \
  } while (0)

// Asserts Result<T> has a value, then moves the inner value into lhs.
// Usage: ASSERT_OK_AND_ASSIGN(auto value, MyFunc());
#define ASSERT_OK_AND_ASSIGN(lhs, expr)                                       \
  auto QUEST_TEST_NAME(quest_test_result_) = (expr);                          \
  ASSERT_TRUE(QUEST_TEST_NAME(quest_test_result_).has_value())                \
      << "Operation failed: " << #expr << "\n"                                \
      << "Error: " << QUEST_TEST_NAME(quest_test_result_).error().ToString(); \
  lhs = std::move(*QUEST_TEST_NAME(quest_test_result_));
