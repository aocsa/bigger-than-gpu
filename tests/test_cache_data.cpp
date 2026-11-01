// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_cache_data.cpp
/// @brief Unit tests for DataBlock implementations.

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "quest/cache/data_block.hpp"
#include "support/test_utils.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::memory;

// =============================================================================
// HostMemoryBlock Tests
// =============================================================================

TEST(HostMemoryBlockTest, ConstructFromRecordBatch) {
  auto batch = quest::testing::MakeTestRecordBatch(100, 4);

  HostMemoryBlock cache("test_id", std::move(batch));

  EXPECT_EQ(cache.id(), "test_id");
  EXPECT_EQ(cache.tier(), MemoryTier::CPU);
  EXPECT_GT(cache.size_in_bytes(), 0U);
  EXPECT_EQ(cache.num_rows(), 100U);
  EXPECT_EQ(cache.num_columns(), 4U);
  EXPECT_TRUE(cache.is_valid());
}

TEST(HostMemoryBlockTest, RecordBatchAccess) {
  auto batch = quest::testing::MakeTestRecordBatch(50, 2);

  HostMemoryBlock cache("batch_access", std::move(batch));

  EXPECT_EQ(cache.id(), "batch_access");
  EXPECT_EQ(cache.num_rows(), 50U);
  EXPECT_EQ(cache.num_columns(), 2U);
  EXPECT_TRUE(cache.is_valid());

  const auto* record_batch = cache.record_batch();
  ASSERT_NE(record_batch, nullptr);
  EXPECT_EQ(record_batch->num_rows(), 50U);
}

TEST(HostMemoryBlockTest, Release) {
  auto batch = quest::testing::MakeTestRecordBatch(10, 1);
  HostMemoryBlock cache("release_test", std::move(batch));

  EXPECT_TRUE(cache.is_valid());
  EXPECT_GT(cache.size_in_bytes(), 0U);

  cache.Release();

  EXPECT_FALSE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 0U);
}

TEST(HostMemoryBlockTest, MoveConstruction) {
  auto batch = quest::testing::MakeTestRecordBatch(20, 3);
  HostMemoryBlock cache1("move_test", std::move(batch));

  auto original_rows = cache1.num_rows();
  auto original_cols = cache1.num_columns();

  HostMemoryBlock cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_test");
  EXPECT_EQ(cache2.num_rows(), original_rows);
  EXPECT_EQ(cache2.num_columns(), original_cols);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());
}

TEST(HostMemoryBlockTest, MoveAssignment) {
  auto batch1 = quest::testing::MakeTestRecordBatch(5, 1);
  auto batch2 = quest::testing::MakeTestRecordBatch(10, 2);

  HostMemoryBlock cache1("cache1", std::move(batch1));
  HostMemoryBlock cache2("cache2", std::move(batch2));

  cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "cache1");
  EXPECT_EQ(cache2.num_rows(), 5U);
  EXPECT_FALSE(cache1.is_valid());
}

TEST(HostMemoryBlockTest, RecordBatchIntegrity) {
  auto batch = quest::testing::MakeTestRecordBatch(8, 2);

  HostMemoryBlock cache("integrity_test", std::move(batch));

  const auto* record_batch = cache.record_batch();
  ASSERT_NE(record_batch, nullptr);
  EXPECT_EQ(record_batch->num_rows(), 8U);
  EXPECT_EQ(record_batch->num_columns(), 2U);
}

// =============================================================================
// DiskBlock Tests
// =============================================================================

class DiskBlockTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // ctest runs each case in its own process, concurrently; a shared dir would be torn down
    // under a sibling.
    temp_dir_ = std::filesystem::temp_directory_path() / "test_cache_data" /
                ::testing::UnitTest::GetInstance()->current_test_info()->name();
    std::filesystem::create_directories(temp_dir_);
  }

  void TearDown() override { std::filesystem::remove_all(temp_dir_); }

  std::filesystem::path CreateTempFile(const std::string& name,
                                       const std::string& content) {
    auto path = temp_dir_ / name;
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path;
  }

  std::filesystem::path temp_dir_;
};

TEST_F(DiskBlockTest, Construction) {
  auto path = CreateTempFile("test.bin", std::string(512, 'X'));

  DiskBlock cache("disk_test", path, 512, 25, 5);

  EXPECT_EQ(cache.id(), "disk_test");
  EXPECT_EQ(cache.tier(), MemoryTier::Disk);
  EXPECT_EQ(cache.size_in_bytes(), 512U);
  EXPECT_EQ(cache.num_rows(), 25U);
  EXPECT_EQ(cache.num_columns(), 5U);
  EXPECT_EQ(cache.file_path(), path);
  EXPECT_TRUE(cache.is_valid());
}

TEST_F(DiskBlockTest, InvalidPath) {
  DiskBlock cache("invalid", "/nonexistent/path/file.bin", 100, 10, 2);
  EXPECT_FALSE(cache.is_valid());
}

// Note: DiskBlock::ToCpu() now expects serialized RecordBatch format.
// Full round-trip testing (HostMemoryBlock->ToDisk->ToCpu) requires
// using RecordBatchSerializer which is tested in integration tests.

TEST_F(DiskBlockTest, MoveConstruction) {
  auto path = CreateTempFile("move.bin", "test data");

  DiskBlock cache1("move_disk", path, 9, 1, 1);
  DiskBlock cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_disk");
  EXPECT_EQ(cache2.file_path(), path);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());
}

TEST_F(DiskBlockTest, LargeFile) {
  std::string large_content(1024 * 1024, 'A');  // 1 MB
  auto path = CreateTempFile("large.bin", large_content);

  DiskBlock cache("large_test", path, large_content.size(), 10000, 10);

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 1024U * 1024U);
  // Note: ToCpu() would fail here since the file is not serialized RecordBatch format
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(DataBlockEdgeCaseTest, EmptyRecordBatch) {
  // Create an empty RecordBatch (0 rows)
  auto batch = quest::testing::MakeTestRecordBatch(0, 1);
  HostMemoryBlock cache("empty", std::move(batch));

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.num_rows(), 0U);
}

TEST(DataBlockEdgeCaseTest, SingleRowRecordBatch) {
  auto batch = quest::testing::MakeTestRecordBatch(1, 1);
  HostMemoryBlock cache("single_row", std::move(batch));

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.num_rows(), 1U);
  EXPECT_EQ(cache.num_columns(), 1U);
}

TEST(DataBlockEdgeCaseTest, DoubleRelease) {
  auto batch = quest::testing::MakeTestRecordBatch(4, 1);
  HostMemoryBlock cache("double_release", std::move(batch));

  cache.Release();
  EXPECT_FALSE(cache.is_valid());

  // Second release should be safe (no-op)
  cache.Release();
  EXPECT_FALSE(cache.is_valid());
}
