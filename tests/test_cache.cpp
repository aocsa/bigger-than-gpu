// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_cache.cpp
/// @brief Unit tests for cache module: CacheData, BatchHolder, CacheMessage.

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/cache/cache_data.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::memory;

// =============================================================================
// CPUCacheData Tests
// =============================================================================

TEST(CPUCacheDataTest, FromBuffer) {
  std::vector<std::byte> data(1024);
  std::memset(data.data(), 0xAB, data.size());

  CPUCacheData cache("test_id", std::move(data), 100, 4);

  EXPECT_EQ(cache.id(), "test_id");
  EXPECT_EQ(cache.tier(), MemoryTier::CPU);
  EXPECT_EQ(cache.size_in_bytes(), 1024);
  EXPECT_EQ(cache.num_rows(), 100);
  EXPECT_EQ(cache.num_columns(), 4);
  EXPECT_TRUE(cache.is_valid());
}

TEST(CPUCacheDataTest, Preallocated) {
  CPUCacheData cache("preallocated", 2048, 50, 2);

  EXPECT_EQ(cache.id(), "preallocated");
  EXPECT_EQ(cache.size_in_bytes(), 2048);
  EXPECT_EQ(cache.num_rows(), 50);
  EXPECT_EQ(cache.num_columns(), 2);
  EXPECT_TRUE(cache.is_valid());

  // Should be able to access mutable data
  auto span = cache.mutable_data();
  EXPECT_EQ(span.size(), 2048);
}

TEST(CPUCacheDataTest, Release) {
  std::vector<std::byte> data(512);
  CPUCacheData cache("release_test", std::move(data), 10, 1);

  EXPECT_TRUE(cache.is_valid());
  cache.release();
  EXPECT_FALSE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 0);
}

TEST(CPUCacheDataTest, Move) {
  std::vector<std::byte> data(256);
  std::memset(data.data(), 0xCD, data.size());
  CPUCacheData cache1("move_test", std::move(data), 20, 3);

  CPUCacheData cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_test");
  EXPECT_EQ(cache2.size_in_bytes(), 256);
  EXPECT_EQ(cache2.num_rows(), 20);
  EXPECT_EQ(cache2.num_columns(), 3);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());
}

TEST(CPUCacheDataTest, SpanAccess) {
  std::vector<std::byte> data(128);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<std::byte>(i % 256);
  }

  CPUCacheData cache("span_test", std::move(data), 8, 2);

  auto span = cache.data();
  EXPECT_EQ(span.size(), 128);
  EXPECT_EQ(static_cast<uint8_t>(span[0]), 0);
  EXPECT_EQ(static_cast<uint8_t>(span[127]), 127);
}

// =============================================================================
// DiskCacheData Tests
// =============================================================================

TEST(DiskCacheDataTest, Basic) {
  // Create a temp file
  auto temp_path = std::filesystem::temp_directory_path() / "test_cache_file.bin";
  {
    std::ofstream file(temp_path, std::ios::binary);
    std::vector<char> data(512, 'X');
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  DiskCacheData cache("disk_test", temp_path, 512, 25, 5);

  EXPECT_EQ(cache.id(), "disk_test");
  EXPECT_EQ(cache.tier(), MemoryTier::Disk);
  EXPECT_EQ(cache.size_in_bytes(), 512);
  EXPECT_EQ(cache.num_rows(), 25);
  EXPECT_EQ(cache.num_columns(), 5);
  EXPECT_EQ(cache.file_path(), temp_path);
  EXPECT_TRUE(cache.is_valid());

  std::filesystem::remove(temp_path);
}

TEST(DiskCacheDataTest, ToCPU) {
  // Create a temp file with known content
  auto temp_path = std::filesystem::temp_directory_path() / "test_to_cpu.bin";
  {
    std::ofstream file(temp_path, std::ios::binary);
    std::string content = "Hello, cache data!";
    file.write(content.data(), static_cast<std::streamsize>(content.size()));
  }

  DiskCacheData disk_cache("to_cpu_test", temp_path, 18, 1, 1);
  auto result = disk_cache.to_cpu();

  ASSERT_TRUE(result.has_value());
  auto cpu_data = result.value();
  EXPECT_EQ(cpu_data->tier(), MemoryTier::CPU);
  EXPECT_EQ(cpu_data->size_in_bytes(), 18);
  EXPECT_EQ(cpu_data->num_rows(), 1);

  // Verify content
  auto* cpu_cache = dynamic_cast<CPUCacheData*>(cpu_data.get());
  ASSERT_NE(cpu_cache, nullptr);
  auto span = cpu_cache->data();
  std::string loaded(reinterpret_cast<const char*>(span.data()), span.size());
  EXPECT_EQ(loaded, "Hello, cache data!");

  // Clean up temp file manually since DiskCacheData still owns it
  std::filesystem::remove(temp_path);
}

TEST(DiskCacheDataTest, Invalid) {
  // Non-existent file
  DiskCacheData cache("invalid", "/nonexistent/path/file.bin", 100, 10, 2);
  EXPECT_FALSE(cache.is_valid());
}

TEST(DiskCacheDataTest, Move) {
  auto temp_path = std::filesystem::temp_directory_path() / "test_move.bin";
  {
    std::ofstream file(temp_path, std::ios::binary);
    file << "test data";
  }

  DiskCacheData cache1("move_disk", temp_path, 9, 1, 1);
  DiskCacheData cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_disk");
  EXPECT_EQ(cache2.file_path(), temp_path);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());

  std::filesystem::remove(temp_path);
}

// =============================================================================
// CacheMessage Tests
// =============================================================================

TEST(CacheMessageTest, Basic) {
  std::vector<std::byte> data(256);
  auto cache_data = std::make_shared<CPUCacheData>("cache_id", std::move(data), 32, 4);

  CacheMessage msg("msg_id", cache_data);

  EXPECT_EQ(msg.id(), "msg_id");
  EXPECT_EQ(msg.size_in_bytes(), 256);
  EXPECT_EQ(msg.num_rows(), 32);
  EXPECT_NE(msg.data(), nullptr);
}

TEST(CacheMessageTest, NullData) {
  CacheMessage msg("empty", nullptr);

  EXPECT_EQ(msg.id(), "empty");
  EXPECT_EQ(msg.size_in_bytes(), 0);
  EXPECT_EQ(msg.num_rows(), 0);
  EXPECT_EQ(msg.data(), nullptr);
}

// =============================================================================
// BatchHolder Tests
// =============================================================================

TEST(BatchHolderTest, Construction) {
  BatchHolder holder(1);

  EXPECT_EQ(holder.id(), 1);
  EXPECT_EQ(holder.type(), BatchHolderType::Simple);
  EXPECT_EQ(holder.batch_count(), 0);
  EXPECT_EQ(holder.bytes_added(), 0);
  EXPECT_EQ(holder.rows_added(), 0);
  EXPECT_FALSE(holder.has_next());
  EXPECT_FALSE(holder.is_finished());
}

TEST(BatchHolderTest, AddAndPull) {
  BatchHolder holder(2);

  // Create and add a message
  std::vector<std::byte> data(1024);
  auto cache_data = std::make_shared<CPUCacheData>("data1", std::move(data), 100, 2);
  auto msg = std::make_unique<CacheMessage>("msg1", cache_data);

  auto add_status = holder.add(std::move(msg));
  EXPECT_TRUE(add_status.ok());
  EXPECT_EQ(holder.batch_count(), 1);
  EXPECT_EQ(holder.bytes_added(), 1024);
  EXPECT_EQ(holder.rows_added(), 100);
  EXPECT_TRUE(holder.has_next());

  // Pull the message
  holder.finish();  // Must finish to allow blocking pop to return
  auto result = holder.pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->id(), "msg1");
  EXPECT_EQ(holder.batch_count(), 0);
}

TEST(BatchHolderTest, TryPull) {
  BatchHolder holder(3);

  // Try pull from empty queue
  auto empty_result = holder.try_pull();
  EXPECT_FALSE(empty_result.has_value());

  // Add a message
  std::vector<std::byte> data(512);
  auto cache_data = std::make_shared<CPUCacheData>("data2", std::move(data), 50, 1);
  (void)holder.add("msg2", cache_data);

  // Try pull should succeed
  auto result = holder.try_pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->id(), "msg2");
}

TEST(BatchHolderTest, Finish) {
  BatchHolder holder(4);

  // Add some messages
  for (int i = 0; i < 3; ++i) {
    std::vector<std::byte> data(100);
    auto cache_data = std::make_shared<CPUCacheData>(
        "data" + std::to_string(i), std::move(data), 10, 1);
    (void)holder.add("msg" + std::to_string(i), cache_data);
  }

  EXPECT_EQ(holder.batch_count(), 3);
  EXPECT_FALSE(holder.is_finished());

  holder.finish();

  // is_finished returns true only when finished AND empty
  EXPECT_FALSE(holder.is_finished());  // Still has messages

  // Pull all messages
  for (int i = 0; i < 3; ++i) {
    auto result = holder.pull();
    ASSERT_TRUE(result.has_value());
  }

  EXPECT_TRUE(holder.is_finished());  // Now finished and empty
}

TEST(BatchHolderTest, NullMessage) {
  BatchHolder holder(5);

  auto status = holder.add(nullptr);
  EXPECT_FALSE(status.ok());
}

TEST(BatchHolderTest, MultipleBatches) {
  BatchHolder holder(6);

  // Add multiple batches
  for (int i = 0; i < 5; ++i) {
    std::vector<std::byte> data(200);
    auto cache_data = std::make_shared<CPUCacheData>(
        "multi" + std::to_string(i), std::move(data), 20, 2);
    (void)holder.add("batch" + std::to_string(i), cache_data);
  }

  EXPECT_EQ(holder.batch_count(), 5);
  EXPECT_EQ(holder.bytes_added(), 1000);  // 5 * 200
  EXPECT_EQ(holder.rows_added(), 100);     // 5 * 20

  holder.finish();

  // Pull all in order
  for (int i = 0; i < 5; ++i) {
    auto result = holder.pull();
    ASSERT_TRUE(result.has_value());
    std::string expected = "batch" + std::to_string(i);
    EXPECT_EQ(result.value()->id(), expected);
  }
}

// =============================================================================
// ConcatenatingBatchHolder Tests
// =============================================================================

TEST(ConcatenatingBatchHolderTest, Construction) {
  ConcatenatingBatchHolder holder(10);

  EXPECT_EQ(holder.id(), 10);
  EXPECT_EQ(holder.type(), BatchHolderType::Concatenating);
  EXPECT_EQ(holder.batch_count(), 0);
}

TEST(ConcatenatingBatchHolderTest, SingleMessage) {
  ConcatenatingBatchHolder holder(11);

  std::vector<std::byte> data(256);
  std::memset(data.data(), 0xAA, data.size());
  auto cache_data = std::make_shared<CPUCacheData>("single", std::move(data), 16, 2);
  (void)holder.add("single_msg", cache_data);

  holder.finish();

  auto result = holder.pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->size_in_bytes(), 256);
}

TEST(ConcatenatingBatchHolderTest, MultipleMessages) {
  ConcatenatingBatchHolder holder(12);

  // Add three messages with different data
  for (int i = 0; i < 3; ++i) {
    std::vector<std::byte> data(100);
    std::memset(data.data(), i, data.size());
    auto cache_data = std::make_shared<CPUCacheData>(
        "concat" + std::to_string(i), std::move(data), 10, 1);
    (void)holder.add("concat_msg" + std::to_string(i), cache_data);
  }

  EXPECT_EQ(holder.batch_count(), 3);
  EXPECT_EQ(holder.bytes_added(), 300);
  EXPECT_EQ(holder.rows_added(), 30);

  holder.finish();

  // Pull should return concatenated result
  auto result = holder.pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->size_in_bytes(), 300);  // All concatenated

  // Queue should now be empty
  EXPECT_EQ(holder.batch_count(), 0);
  EXPECT_TRUE(holder.is_finished());
}

TEST(ConcatenatingBatchHolderTest, TryPullNotFinished) {
  ConcatenatingBatchHolder holder(13);

  std::vector<std::byte> data(128);
  auto cache_data = std::make_shared<CPUCacheData>("test", std::move(data), 8, 1);
  (void)holder.add("test_msg", cache_data);

  // Try pull before finish should fail
  auto result = holder.try_pull();
  EXPECT_FALSE(result.has_value());

  holder.finish();

  // Now it should work
  result = holder.try_pull();
  EXPECT_TRUE(result.has_value());
}

// =============================================================================
// make_batch_holder Factory Tests
// =============================================================================

TEST(MakeBatchHolderTest, Simple) {
  auto holder = make_batch_holder(100, BatchHolderType::Simple);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 100);
  EXPECT_EQ(holder->type(), BatchHolderType::Simple);
}

TEST(MakeBatchHolderTest, Concatenating) {
  auto holder = make_batch_holder(101, BatchHolderType::Concatenating);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 101);
  EXPECT_EQ(holder->type(), BatchHolderType::Concatenating);
}

TEST(MakeBatchHolderTest, WithConfig) {
  BatchHolderConfig config;
  config.flow_control_batches_threshold = 10;
  config.flow_control_bytes_threshold = 1024 * 1024;
  config.auto_spill = false;

  auto holder = make_batch_holder(102, BatchHolderType::Simple, config);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 102);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(BatchHolderThreadSafetyTest, ProducerConsumer) {
  BatchHolder holder(200);

  // Producer thread
  auto producer = std::async(std::launch::async, [&holder]() {
    for (int i = 0; i < 10; ++i) {
      std::vector<std::byte> data(64);
      auto cache_data = std::make_shared<CPUCacheData>(
          "prod" + std::to_string(i), std::move(data), 4, 1);
      (void)holder.add("prod_msg" + std::to_string(i), cache_data);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    holder.finish();
  });

  // Consumer thread
  auto consumer = std::async(std::launch::async, [&holder]() {
    int count = 0;
    while (!holder.is_finished() || holder.has_next()) {
      auto result = holder.try_pull();
      if (result.has_value()) {
        ++count;
      } else {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    }
    return count;
  });

  producer.get();
  int consumed = consumer.get();
  EXPECT_EQ(consumed, 10);
}

TEST(BatchHolderThreadSafetyTest, ConcurrentAdds) {
  BatchHolder holder(201);

  constexpr int num_threads = 4;
  constexpr int messages_per_thread = 25;

  std::vector<std::future<void>> futures;
  for (int t = 0; t < num_threads; ++t) {
    futures.push_back(std::async(std::launch::async, [&holder, t]() {
      for (int i = 0; i < messages_per_thread; ++i) {
        std::vector<std::byte> data(32);
        auto cache_data = std::make_shared<CPUCacheData>(
            std::to_string(t) + "_" + std::to_string(i), std::move(data), 2, 1);
        (void)holder.add(std::to_string(t) + "_msg_" + std::to_string(i), cache_data);
      }
    }));
  }

  for (auto& f : futures) {
    f.get();
  }

  EXPECT_EQ(holder.batch_count(), num_threads * messages_per_thread);
  holder.finish();

  // Consume all
  int count = 0;
  while (holder.has_next()) {
    auto result = holder.try_pull();
    if (result.has_value()) ++count;
  }
  EXPECT_EQ(count, num_threads * messages_per_thread);
}
