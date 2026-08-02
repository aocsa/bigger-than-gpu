#ifndef QUEST_TESTS_UTILS_HPP_
#define QUEST_TESTS_UTILS_HPP_

#include <gtest/gtest.h>
#include <utility>

#include "quest/datasource/column_vector.hpp"
#include "quest/datasource/record_batch.hpp"
#include "quest/datasource/schema.hpp"
#include "quest/datasource/types.hpp"

// Helper macro to generate unique variable names
#define _QUEST_GTEST_CONCAT_IMPL(x, y) x##y
#define _QUEST_GTEST_CONCAT(x, y) _QUEST_GTEST_CONCAT_IMPL(x, y)
#define _QUEST_GTEST_NAME(x) _QUEST_GTEST_CONCAT(x, __LINE__)

namespace quest::testing {

/// @brief Single int32 column named "value"; values are 0, 1, 2, …
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

/// @brief Multiple int32 columns named "col0", "col1", etc.
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

// Fails the test if Status/Result is not OK.
#define ASSERT_OK(expr)                                                    \
  do {                                                                     \
    auto _res = (expr);                                                    \
    ASSERT_TRUE(_res.has_value())                                          \
        << "Operation failed: " << #expr << "\n"                           \
        << "Error: " << _res.error().ToString();                           \
  } while (0)

// Asserts Result<T> has a value, then moves the inner value into lhs.
// Usage: ASSERT_OK_AND_ASSIGN(auto value, MyFunc());
#define ASSERT_OK_AND_ASSIGN(lhs, expr)                                    \
  auto _QUEST_GTEST_NAME(_result) = (expr);                                \
  ASSERT_TRUE(_QUEST_GTEST_NAME(_result).has_value())                      \
      << "Operation failed: " << #expr << "\n"                             \
      << "Error: " << _QUEST_GTEST_NAME(_result).error().ToString();       \
  lhs = std::move(*_QUEST_GTEST_NAME(_result));

#endif  // QUEST_TESTS_UTILS_HPP_
