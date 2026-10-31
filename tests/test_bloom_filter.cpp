// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

/// @file test_bloom_filter.cpp
/// @brief Membership guarantees and batched join equivalence, including false positives.

#include <gtest/gtest.h>

#include <limits>
#include <vector>

#include "quest/execution/bloom_filter.hpp"
#include "../examples/bloom_join/tpch_bloom_join.hpp"

namespace {
using quest::execution::BloomFilter;
using quest::examples::Lineitem;
using quest::examples::Part;
using quest::examples::TpchBloomJoin;

TEST(BloomFilter, ValidatesConfiguration) {
  EXPECT_FALSE(BloomFilter::Create(0));
  EXPECT_FALSE(BloomFilter::Create(63));
  EXPECT_FALSE(BloomFilter::Create(65));
  EXPECT_FALSE(BloomFilter::Create(64, 0));
  EXPECT_FALSE(BloomFilter::Create(64, 17));
  EXPECT_FALSE(BloomFilter::Create(std::numeric_limits<std::size_t>::max()));
  auto bloom = BloomFilter::Create(1024);
  ASSERT_TRUE(bloom);
  EXPECT_EQ(bloom->GetSizeBytes(), 128u);
}

TEST(BloomFilter, EmptyDuplicatesAndExtremeKeysHaveNoFalseNegatives) {
  auto bloom = BloomFilter::Create(16384);
  ASSERT_TRUE(bloom);
  EXPECT_FALSE(bloom->MayContain(0));
  EXPECT_FALSE(bloom->MayContain(UINT64_MAX));
  for (uint64_t key = 0; key < 1000; ++key) {
    bloom->Insert(key);
    bloom->Insert(key);
  }
  bloom->Insert(UINT64_MAX);
  for (uint64_t key = 0; key < 1000; ++key) EXPECT_TRUE(bloom->MayContain(key));
  EXPECT_TRUE(bloom->MayContain(UINT64_MAX));
  std::size_t negatives = 0;
  for (uint64_t key = 1000; key < 2000; ++key) negatives += !bloom->MayContain(key);
  EXPECT_GT(negatives, 900u);  // Loose deterministic sanity check, not a probabilistic guarantee.
}

TEST(BloomJoin, RejectsIncompleteBuildAndLateInsertions) {
  auto join = TpchBloomJoin::Create("Brand#23");
  ASSERT_TRUE(join);
  EXPECT_FALSE(join->ProbeBatch({}));
  EXPECT_TRUE(join->AddBuildBatch({}));
  join->FinalizeBuild();
  join->FinalizeBuild();
  EXPECT_FALSE(join->AddBuildBatch({}));
  auto empty = join->ProbeBatch({});
  ASSERT_TRUE(empty);
  EXPECT_TRUE(empty->rows.empty());
  const std::vector<Lineitem> items{{1, 10, 100}};
  auto result = join->ProbeBatch(items);
  ASSERT_TRUE(result);
  EXPECT_TRUE(result->rows.empty());
  EXPECT_EQ(result->bloom_rejected, 1u);
}

TEST(BloomJoin, BatchesBrandPredicateAndDuplicateMultiplicityMatchNestedLoop) {
  const std::vector<Part> parts{{10, "Brand#23"}, {20, "Brand#12"},
                              {10, "Brand#23"}, {-1, "Brand#23"}};
  const std::vector<Lineitem> items{{1, 10, 100}, {2, 20, 200}, {3, 30, 300},
                                  {4, 10, 400}, {5, -1, 500}};
  auto join = TpchBloomJoin::Create("Brand#23");
  ASSERT_TRUE(join);
  const auto build = std::span<const Part>(parts);
  ASSERT_TRUE(join->AddBuildBatch(build.first(2)));
  ASSERT_TRUE(join->AddBuildBatch(build.subspan(2)));
  join->FinalizeBuild();
  std::vector<Lineitem> actual;
  for (const auto& item : items) {
    auto result = join->ProbeBatch(std::span<const Lineitem>(&item, 1));
    ASSERT_TRUE(result);
    actual.insert(actual.end(), result->rows.begin(), result->rows.end());
  }
  std::vector<Lineitem> expected;
  for (const auto& item : items) {
    for (const auto& part : parts) {
      if (part.brand == "Brand#23" && item.part_key == part.part_key) expected.push_back(item);
    }
  }
  EXPECT_EQ(actual, expected);
  auto baseline = join->ProbeBatch(items, false);
  ASSERT_TRUE(baseline);
  EXPECT_EQ(baseline->rows, expected);
  EXPECT_EQ(baseline->exact_lookups, items.size());
}

TEST(BloomJoin, SaturatedFilterStillProducesExactResults) {
  auto join = TpchBloomJoin::Create("Brand#23", 64, 7);
  ASSERT_TRUE(join);
  std::vector<Part> parts;
  for (int64_t key = 0; key < 1000; ++key) parts.push_back({key, "Brand#23"});
  ASSERT_TRUE(join->AddBuildBatch(parts));
  join->FinalizeBuild();
  const std::vector<Lineitem> items{{1, 1, 100}, {2, 1001, 200}, {3, 2001, 300}};
  auto filtered = join->ProbeBatch(items);
  auto baseline = join->ProbeBatch(items, false);
  ASSERT_TRUE(filtered);
  ASSERT_TRUE(baseline);
  EXPECT_EQ(filtered->rows, baseline->rows);
  EXPECT_EQ(filtered->rows.size(), 1u);
  // Prove this test exercised positives for absent keys, not just true matches.
  EXPECT_GT(filtered->exact_lookups, filtered->rows.size());
}
}  // namespace
