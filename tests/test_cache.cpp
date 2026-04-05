// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_cache.cpp
/// @brief Unit tests for cache module: DataBlock, BatchHolder.

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
#include "quest/cache/data_block.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::memory;

// =============================================================================
// HostMemoryBlock Tests
// =============================================================================

TEST(HostMemoryBlockTest, FromRecordBatch) {
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

  // Should be able to access the record batch
  const auto* rb = cache.record_batch();
  ASSERT_NE(rb, nullptr);
  EXPECT_EQ(rb->num_rows(), 50U);
}

TEST(HostMemoryBlockTest, ReleaseTest) {
  auto batch = quest::testing::MakeTestRecordBatch(10, 1);
  HostMemoryBlock cache("release_test", std::move(batch));

  EXPECT_TRUE(cache.is_valid());
  cache.Release();
  EXPECT_FALSE(cache.is_valid());
  EXPECT_EQ(cache.size_in_bytes(), 0U);
}

TEST(HostMemoryBlockTest, Move) {
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

TEST(HostMemoryBlockTest, RecordBatchIntegrity) {
  auto batch = quest::testing::MakeTestRecordBatch(8, 2);

  HostMemoryBlock cache("integrity_test", std::move(batch));

  const auto* rb = cache.record_batch();
  ASSERT_NE(rb, nullptr);
  EXPECT_EQ(rb->num_rows(), 8U);
  EXPECT_EQ(rb->num_columns(), 2U);
}

// =============================================================================
// DiskBlock Tests
// =============================================================================

TEST(DiskBlockTest, Basic) {
  // Create a temp file
  auto temp_path =
      std::filesystem::temp_directory_path() / "test_cache_file.bin";
  {
    std::ofstream file(temp_path, std::ios::binary);
    std::vector<char> data(512, 'X');
    file.write(data.data(), static_cast<std::streamsize>(data.size()));
  }

  DiskBlock cache("disk_test", temp_path, 512, 25, 5);

  EXPECT_EQ(cache.id(), "disk_test");
  EXPECT_EQ(cache.tier(), MemoryTier::Disk);
  EXPECT_EQ(cache.size_in_bytes(), 512);
  EXPECT_EQ(cache.num_rows(), 25);
  EXPECT_EQ(cache.num_columns(), 5);
  EXPECT_EQ(cache.file_path(), temp_path);
  EXPECT_TRUE(cache.is_valid());

  std::filesystem::remove(temp_path);
}

// Note: DiskBlock::ToCpu() now expects serialized RecordBatch format.
// Testing the full round-trip (HostMemoryBlock->ToDisk->ToCpu) is done
// in test_cache_data.cpp or integration tests.

TEST(DiskBlockTest, Invalid) {
  // Non-existent file
  DiskBlock cache("invalid", "/nonexistent/path/file.bin", 100, 10, 2);
  EXPECT_FALSE(cache.is_valid());
}

TEST(DiskBlockTest, Move) {
  auto temp_path = std::filesystem::temp_directory_path() / "test_move.bin";
  {
    std::ofstream file(temp_path, std::ios::binary);
    file << "test data";
  }

  DiskBlock cache1("move_disk", temp_path, 9, 1, 1);
  DiskBlock cache2 = std::move(cache1);

  EXPECT_EQ(cache2.id(), "move_disk");
  EXPECT_EQ(cache2.file_path(), temp_path);
  EXPECT_TRUE(cache2.is_valid());
  EXPECT_FALSE(cache1.is_valid());

  std::filesystem::remove(temp_path);
}


// =============================================================================
// BatchHolder Tests
// =============================================================================

TEST(BatchHolderTest, Construction) {
  BatchHolder holder(1);

  EXPECT_EQ(holder.id(), 1);
  EXPECT_EQ(holder.type(), BatchHolderType::kSimple);
  EXPECT_EQ(holder.batch_count(), 0);
  EXPECT_EQ(holder.bytes_added(), 0);
  EXPECT_EQ(holder.rows_added(), 0);
  EXPECT_FALSE(holder.HasNext());
  EXPECT_FALSE(holder.IsFinished());
}

TEST(BatchHolderTest, AddAndPull) {
  BatchHolder holder(2);

  // Create and add a data block
  auto batch = quest::testing::MakeTestRecordBatch(100, 2);
  auto cache_data =
      std::make_shared<HostMemoryBlock>("data1", std::move(batch));

  auto add_status = holder.Add(cache_data);
  EXPECT_TRUE(add_status.ok());
  EXPECT_EQ(holder.batch_count(), 1U);
  EXPECT_GT(holder.bytes_added(), 0U);
  EXPECT_EQ(holder.rows_added(), 100U);
  EXPECT_TRUE(holder.HasNext());

  // Pull the data block
  holder.Finish();  // Must finish to allow blocking pop to return
  auto result = holder.Pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->id(), "data1");
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderTest, TryPull) {
  BatchHolder holder(3);

  // Try pull from empty queue
  auto empty_result = holder.TryPull();
  EXPECT_FALSE(empty_result.has_value());

  // Add a data block
  auto batch = quest::testing::MakeTestRecordBatch(50, 1);
  auto cache_data =
      std::make_shared<HostMemoryBlock>("data2", std::move(batch));
  (void)holder.Add(cache_data);

  // Try pull should succeed
  auto result = holder.TryPull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->id(), "data2");
}

TEST(BatchHolderTest, Finish) {
  BatchHolder holder(4);

  // Add some data blocks
  for (int i = 0; i < 3; ++i) {
    auto batch = quest::testing::MakeTestRecordBatch(10, 1);
    auto cache_data = std::make_shared<HostMemoryBlock>(
        "data" + std::to_string(i), std::move(batch));
    (void)holder.Add(cache_data);
  }

  EXPECT_EQ(holder.batch_count(), 3);
  EXPECT_FALSE(holder.IsFinished());

  holder.Finish();

  // IsFinished returns true only when finished AND empty
  EXPECT_FALSE(holder.IsFinished());  // Still has data blocks

  // Pull all data blocks
  for (int i = 0; i < 3; ++i) {
    auto result = holder.Pull();
    ASSERT_TRUE(result.has_value());
  }

  EXPECT_TRUE(holder.IsFinished());  // Now finished and empty
}

TEST(BatchHolderTest, NullMessage) {
  BatchHolder holder(5);

  auto status = holder.Add(nullptr);
  EXPECT_FALSE(status.ok());
}

TEST(BatchHolderTest, MultipleBatches) {
  BatchHolder holder(6);

  // Add multiple batches
  for (int i = 0; i < 5; ++i) {
    auto batch = quest::testing::MakeTestRecordBatch(20, 2);
    auto cache_data = std::make_shared<HostMemoryBlock>(
        "batch" + std::to_string(i), std::move(batch));
    (void)holder.Add(cache_data);
  }

  EXPECT_EQ(holder.batch_count(), 5U);
  EXPECT_GT(holder.bytes_added(), 0U);
  EXPECT_EQ(holder.rows_added(), 100U);    // 5 * 20

  holder.Finish();

  // Pull all in order
  for (int i = 0; i < 5; ++i) {
    auto result = holder.Pull();
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
  EXPECT_EQ(holder.type(), BatchHolderType::kConcatenating);
  EXPECT_EQ(holder.batch_count(), 0);
}

TEST(ConcatenatingBatchHolderTest, SingleMessage) {
  ConcatenatingBatchHolder holder(11);

  auto batch = quest::testing::MakeTestRecordBatch(16, 2);
  auto cache_data =
      std::make_shared<HostMemoryBlock>("single", std::move(batch));
  (void)holder.Add(cache_data);

  holder.Finish();

  auto result = holder.Pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_GT(result.value()->size_in_bytes(), 0U);
}

TEST(ConcatenatingBatchHolderTest, SingleMessagePull) {
  // ConcatenatingBatchHolder returns single block as-is when only one is added
  ConcatenatingBatchHolder holder(12);

  auto batch = quest::testing::MakeTestRecordBatch(10, 1);
  auto original_size = batch.num_rows();
  auto cache_data = std::make_shared<HostMemoryBlock>("concat0", std::move(batch));
  (void)holder.Add(cache_data);

  EXPECT_EQ(holder.batch_count(), 1U);
  EXPECT_EQ(holder.rows_added(), original_size);

  holder.Finish();

  // Pull should return the single block
  auto result = holder.Pull();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->num_rows(), original_size);

  // Queue should now be empty
  EXPECT_EQ(holder.batch_count(), 0U);
  EXPECT_TRUE(holder.IsFinished());
}

TEST(ConcatenatingBatchHolderTest, TryPullNotFinished) {
  ConcatenatingBatchHolder holder(13);

  auto batch = quest::testing::MakeTestRecordBatch(8, 1);
  auto cache_data =
      std::make_shared<HostMemoryBlock>("test", std::move(batch));
  (void)holder.Add(cache_data);

  // Try pull before finish should fail
  auto result = holder.TryPull();
  EXPECT_FALSE(result.has_value());

  holder.Finish();

  // Now it should work
  result = holder.TryPull();
  EXPECT_TRUE(result.has_value());
}

// =============================================================================
// MakeBatchHolder Factory Tests
// =============================================================================

TEST(MakeBatchHolderTest, Simple) {
  auto holder = MakeBatchHolder(100, BatchHolderType::kSimple);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 100);
  EXPECT_EQ(holder->type(), BatchHolderType::kSimple);
}

TEST(MakeBatchHolderTest, Concatenating) {
  auto holder = MakeBatchHolder(101, BatchHolderType::kConcatenating);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 101);
  EXPECT_EQ(holder->type(), BatchHolderType::kConcatenating);
}

TEST(MakeBatchHolderTest, WithConfig) {
  BatchHolderConfig config;
  config.flow_control_batches_threshold = 10;
  config.flow_control_bytes_threshold = 1024 * 1024;
  config.auto_spill = false;

  auto holder = MakeBatchHolder(102, BatchHolderType::kSimple, config);

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
      auto batch = quest::testing::MakeTestRecordBatch(4, 1);
      auto cache_data = std::make_shared<HostMemoryBlock>(
          "prod" + std::to_string(i), std::move(batch));
      (void)holder.Add(cache_data);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    holder.Finish();
  });

  // Consumer thread
  auto consumer = std::async(std::launch::async, [&holder]() {
    int count = 0;
    while (!holder.IsFinished() || holder.HasNext()) {
      auto result = holder.TryPull();
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
  constexpr int blocks_per_thread = 25;

  std::vector<std::future<void>> futures;
  for (int thread_idx = 0; thread_idx < num_threads; ++thread_idx) {
    futures.push_back(
        std::async(std::launch::async, [&holder, thread_idx]() {
          for (int i = 0; i < blocks_per_thread; ++i) {
            auto batch = quest::testing::MakeTestRecordBatch(2, 1);
            auto cache_data = std::make_shared<HostMemoryBlock>(
                std::to_string(thread_idx) + "_" + std::to_string(i),
                std::move(batch));
            (void)holder.Add(cache_data);
          }
        }));
  }

  for (auto& fut : futures) {
    fut.get();
  }

  EXPECT_EQ(holder.batch_count(),
            static_cast<size_t>(num_threads * blocks_per_thread));
  holder.Finish();

  // Consume all
  int count = 0;
  while (holder.HasNext()) {
    auto result = holder.TryPull();
    if (result.has_value()) {
      ++count;
    }
  }
  EXPECT_EQ(count, num_threads * blocks_per_thread);
}
