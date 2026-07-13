// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_generator.cpp
/// @brief Unit tests for Generator coroutine.

#include <gtest/gtest.h>

#include <vector>

#include "quest/builder/generator.hpp"

using namespace quest;

// =============================================================================
// Generator Tests
// =============================================================================

TEST(GeneratorTest, BasicIteration) {
  auto gen = []() -> Generator<int> {
    for (int i = 0; i < 5; ++i) {
      co_yield i;
    }
  }();

  std::vector<int> results;
  for (auto value : gen) {
    results.push_back(value);
  }

  EXPECT_EQ(results.size(), 5);
  for (int i = 0; i < 5; ++i) {
    EXPECT_EQ(results[i], i);
  }
}

TEST(GeneratorTest, MoveOnly) {
  auto gen1 = []() -> Generator<int> {
    co_yield 42;
  }();

  Generator<int> gen2 = std::move(gen1);  // Move construction

  auto it = gen2.begin();
  EXPECT_EQ(*it, 42);
}

TEST(GeneratorTest, EmptyGenerator) {
  auto gen = []() -> Generator<int> {
    co_return;  // Yields nothing
  }();

  int count = 0;
  for ([[maybe_unused]] auto value : gen) {
    ++count;
  }
  EXPECT_EQ(count, 0);
}

TEST(GeneratorTest, NextMethod) {
  auto gen = []() -> Generator<int> {
    co_yield 1;
    co_yield 2;
    co_yield 3;
  }();

  auto v1 = gen.next();
  EXPECT_TRUE(v1.has_value());
  EXPECT_EQ(*v1, 1);

  auto v2 = gen.next();
  EXPECT_TRUE(v2.has_value());
  EXPECT_EQ(*v2, 2);

  auto v3 = gen.next();
  EXPECT_TRUE(v3.has_value());
  EXPECT_EQ(*v3, 3);

  auto v4 = gen.next();
  EXPECT_FALSE(v4.has_value());  // No more values
}
