#pragma once
#include <gtest/gtest.h>
#include <utility> // for std::move

// Helper macro to generate unique variable names
#define _QUEST_GTEST_CONCAT_IMPL(x, y) x##y
#define _QUEST_GTEST_CONCAT(x, y) _QUEST_GTEST_CONCAT_IMPL(x, y)
#define _QUEST_GTEST_NAME(x) _QUEST_GTEST_CONCAT(x, __LINE__)

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