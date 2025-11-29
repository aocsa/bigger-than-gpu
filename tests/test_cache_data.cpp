// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_cache_data.cpp
/// @brief Unit tests for CacheData implementations.

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>

#include "quest/cache/cache_data.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::memory;

// =============================================================================
// CPUCacheData Tests
// =============================================================================

TEST(CPUCacheDataTest, ConstructFromBuffer) {
  std::vector<std::byte> data(1024);
  std::memset(data.data(), 0xAB, data.size());

  CPUCacheData cache("test_id", std::move(data), 100, 4);

  EXPECT_EQ(cache.id(), "test_id");
  EXPECT_EQ(cache.tier(), MemoryTier::CPU);
  EXPECT_EQ(cache.size_in_bytes(), 1024U);
  EXPECT_EQ(cache.num_rows(), 100U);
  EXPECT_EQ(cache.num_columns(), 4U);
  EXPECT_TRUE(cache.is_valid());
}

TEST(CPUCacheDataTest, ConstructPreallocated) {
  CPUCacheData cache("preallocated", 2048, 50, 2);

  EXPECT_EQ(cache.id(), "preallocated");
  EXPECT_EQ(cache.size_in_bytes(), 2048U);
  EXPECT_EQ(cache.num_rows(), 50U);
  EXPECT_EQ(cache.num_columns(), 2U);
  EXPECT_TRUE(cache.is_valid());
}

TEST(CPUCacheDataTest, MutableDataAccess) {
  CPUCacheData cache("mutable_test", 512, 10, 1);

  auto span = cache.mutable_data();
  EXPECT_EQ(span.size(), 512U);

  // Write to mutable data
  std::memset(span.data(), 0xFF, span.size());

  // Verify through const access
  auto const_span = cache.data();
  EXPECT_EQ(static_cast<uint8_t>(const_span[0]), 0xFF);
}

TEST(CPUCacheDataTest, Release) {
  std::vector<std::byte> data(512);
  CPUCacheData cache("release_test", std::move(data), 10, 1);

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 512U);

  cache.release();

  EXPECT_FALSE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 0U);
}

TEST(CPUCacheDataTest, MoveConstruction) {
  std::vector<std::byte> data(256);
  std::memset(data.data(), 0xCD, data.size());
  CPUCacheData cache1("move_test", std::move(data), 20, 3);

  CPUCacheData cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_test");
  EXPECT_EQ(cache2.size_in_bytes(), 256U);
  EXPECT_EQ(cache2.num_rows(), 20U);
  EXPECT_EQ(cache2.num_columns(), 3U);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());
}

TEST(CPUCacheDataTest, MoveAssignment) {
  std::vector<std::byte> data1(100);
  std::vector<std::byte> data2(200);

  CPUCacheData cache1("cache1", std::move(data1), 5, 1);
  CPUCacheData cache2("cache2", std::move(data2), 10, 2);

  cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "cache1");
  EXPECT_EQ(cache2.size_in_bytes(), 100U);
  EXPECT_FALSE(cache1.is_valid());
}

TEST(CPUCacheDataTest, DataIntegrity) {
  std::vector<std::byte> data(128);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::byte>(i % 256);
  }

  CPUCacheData cache("integrity_test", std::move(data), 8, 2);

  auto span = cache.data();
  EXPECT_EQ(span.size(), 128U);
  EXPECT_EQ(static_cast<uint8_t>(span[0]), 0);
  EXPECT_EQ(static_cast<uint8_t>(span[127]), 127);
  EXPECT_EQ(static_cast<uint8_t>(span[100]), 100);
}

// =============================================================================
// DiskCacheData Tests
// =============================================================================

class DiskCacheDataTest : public ::testing::Test {
 protected:
  void SetUp() override {
    temp_dir_ = std::filesystem::temp_directory_path() / "test_cache_data";
    std::filesystem::create_directories(temp_dir_);
  }

  void TearDown() override {
    std::filesystem::remove_all(temp_dir_);
  }

  std::filesystem::path CreateTempFile(const std::string& name,
                                       const std::string& content) {
    auto path = temp_dir_ / name;
    std::ofstream file(path, std::ios::binary);
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
    return path;
  }

  std::filesystem::path temp_dir_;
};

TEST_F(DiskCacheDataTest, Construction) {
  auto path = CreateTempFile("test.bin", std::string(512, 'X'));

  DiskCacheData cache("disk_test", path, 512, 25, 5);

  EXPECT_EQ(cache.id(), "disk_test");
  EXPECT_EQ(cache.tier(), MemoryTier::Disk);
  EXPECT_EQ(cache.size_in_bytes(), 512U);
  EXPECT_EQ(cache.num_rows(), 25U);
  EXPECT_EQ(cache.num_columns(), 5U);
  EXPECT_EQ(cache.file_path(), path);
  EXPECT_TRUE(cache.is_valid());
}

TEST_F(DiskCacheDataTest, InvalidPath) {
  DiskCacheData cache("invalid", "/nonexistent/path/file.bin", 100, 10, 2);
  EXPECT_FALSE(cache.is_valid());
}

TEST_F(DiskCacheDataTest, ToCPU) {
  std::string content = "Hello, cache data!";
  auto path = CreateTempFile("to_cpu.bin", content);

  DiskCacheData disk_cache("to_cpu_test", path, content.size(), 1, 1);
  auto result = disk_cache.to_cpu();

  ASSERT_TRUE(result.has_value());

  auto cpu_data = result.value();
  EXPECT_EQ(cpu_data->tier(), MemoryTier::CPU);
  EXPECT_EQ(cpu_data->size_in_bytes(), content.size());
  EXPECT_EQ(cpu_data->num_rows(), 1U);

  // Verify content
  auto* cpu_cache = dynamic_cast<CPUCacheData*>(cpu_data.get());
  ASSERT_NE(cpu_cache, nullptr);

  auto span = cpu_cache->data();
  std::string loaded(reinterpret_cast<const char*>(span.data()), span.size());
  EXPECT_EQ(loaded, content);
}

TEST_F(DiskCacheDataTest, MoveConstruction) {
  auto path = CreateTempFile("move.bin", "test data");

  DiskCacheData cache1("move_disk", path, 9, 1, 1);
  DiskCacheData cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_disk");
  EXPECT_EQ(cache2.file_path(), path);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());
}

TEST_F(DiskCacheDataTest, LargeFile) {
  std::string large_content(1024 * 1024, 'A');  // 1 MB
  auto path = CreateTempFile("large.bin", large_content);

  DiskCacheData cache("large_test", path, large_content.size(), 10000, 10);

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 1024U * 1024U);

  auto result = cache.to_cpu();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->size_in_bytes(), 1024U * 1024U);
}

// =============================================================================
// CacheMessage Tests
// =============================================================================

TEST(CacheMessageTest, Construction) {
  std::vector<std::byte> data(256);
  auto cache_data =
      std::make_shared<CPUCacheData>("cache_id", std::move(data), 32, 4);

  CacheMessage msg("msg_id", cache_data);

  EXPECT_EQ(msg.id(), "msg_id");
  EXPECT_EQ(msg.size_in_bytes(), 256U);
  EXPECT_EQ(msg.num_rows(), 32U);
  EXPECT_NE(msg.data(), nullptr);
}

TEST(CacheMessageTest, NullData) {
  CacheMessage msg("empty", nullptr);

  EXPECT_EQ(msg.id(), "empty");
  EXPECT_EQ(msg.size_in_bytes(), 0U);
  EXPECT_EQ(msg.num_rows(), 0U);
  EXPECT_EQ(msg.data(), nullptr);
}

TEST(CacheMessageTest, SharedOwnership) {
  std::vector<std::byte> data(128);
  auto cache_data =
      std::make_shared<CPUCacheData>("shared", std::move(data), 8, 1);

  CacheMessage msg1("msg1", cache_data);
  CacheMessage msg2("msg2", cache_data);

  EXPECT_EQ(msg1.data(), msg2.data());
  EXPECT_EQ(cache_data.use_count(), 3);  // cache_data + msg1 + msg2
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(CacheDataEdgeCaseTest, EmptyCPUData) {
  std::vector<std::byte> empty_data;
  CPUCacheData cache("empty", std::move(empty_data), 0, 0);

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 0U);
  EXPECT_EQ(cache.data().size(), 0U);
}

TEST(CacheDataEdgeCaseTest, ZeroRowsNonZeroBytes) {
  std::vector<std::byte> data(100);
  CPUCacheData cache("zero_rows", std::move(data), 0, 0);

  EXPECT_TRUE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 100U);
  EXPECT_EQ(cache.num_rows(), 0U);
}

TEST(CacheDataEdgeCaseTest, DoubleRelease) {
  std::vector<std::byte> data(64);
  CPUCacheData cache("double_release", std::move(data), 4, 1);

  cache.release();
  EXPECT_FALSE(cache.is_valid());

  // Second release should be safe (no-op)
  cache.release();
  EXPECT_FALSE(cache.is_valid());
}
