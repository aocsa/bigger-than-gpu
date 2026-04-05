// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_batch_holder.cpp
/// @brief Unit tests for BatchHolder implementations.

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <thread>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::memory;

// =============================================================================
// Helper Functions
// =============================================================================

namespace {

DataBlockPtr MakeTestData(const std::string& id, size_t /*bytes*/, size_t rows) {
  auto batch = quest::testing::MakeTestRecordBatch(rows);
  return std::make_shared<HostMemoryBlock>(id, std::move(batch));
}

}  // namespace

// =============================================================================
// BatchHolder Basic Tests
// =============================================================================

TEST(BatchHolderTest, Construction) {
  BatchHolder holder(1);

  EXPECT_EQ(holder.id(), 1U);
  EXPECT_EQ(holder.type(), BatchHolderType::kSimple);
  EXPECT_EQ(holder.batch_count(), 0U);
  EXPECT_EQ(holder.bytes_added(), 0U);
  EXPECT_EQ(holder.rows_added(), 0U);
  EXPECT_FALSE(holder.HasNext());
  EXPECT_FALSE(holder.IsFinished());
}

TEST(BatchHolderTest, AddDataBlock) {
  BatchHolder holder(1);

  auto data = MakeTestData("data1", 1024, 100);
  auto status = holder.Add(std::move(data));

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(holder.batch_count(), 1U);
  EXPECT_GT(holder.bytes_added(), 0U);  // Size depends on RecordBatch structure
  EXPECT_EQ(holder.rows_added(), 100U);
  EXPECT_TRUE(holder.HasNext());
}

TEST(BatchHolderTest, AddNullMessage) {
  BatchHolder holder(1);

  auto status = holder.Add(nullptr);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderTest, TryPull) {
  BatchHolder holder(1);

  // Empty queue
  auto empty_result = holder.TryPull();
  EXPECT_FALSE(empty_result.has_value());

  // Add data block
  (void)holder.Add(MakeTestData("data1", 256, 10));

  // Pull it
  ASSERT_OK_AND_ASSIGN(auto data, holder.TryPull());
  EXPECT_EQ(data->id(), "data1");
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderTest, PullAfterFinish) {
  BatchHolder holder(1);

  (void)holder.Add(MakeTestData("data1", 256, 10));
  holder.Finish();

  ASSERT_OK_AND_ASSIGN(auto data, holder.Pull());
  EXPECT_EQ(data->id(), "data1");
}

TEST(BatchHolderTest, IsFinished) {
  BatchHolder holder(1);

  EXPECT_FALSE(holder.IsFinished());

  (void)holder.Add(MakeTestData("data1", 100, 5));
  EXPECT_FALSE(holder.IsFinished());

  holder.Finish();
  // Still has data blocks
  EXPECT_FALSE(holder.IsFinished());

  // Pull all data blocks
  (void)holder.Pull();
  // Now finished and empty
  EXPECT_TRUE(holder.IsFinished());
}

TEST(BatchHolderTest, MultipleBatches) {
  BatchHolder holder(1);

  for (int i = 0; i < 5; ++i) {
    (void)holder.Add(MakeTestData("data" + std::to_string(i), 100, 10));
  }

  EXPECT_EQ(holder.batch_count(), 5U);
  EXPECT_GT(holder.bytes_added(), 0U);  // Size depends on RecordBatch structure
  EXPECT_EQ(holder.rows_added(), 50U);

  holder.Finish();

  // Pull in order (FIFO)
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK_AND_ASSIGN(auto data, holder.Pull());
    EXPECT_EQ(data->id(), "data" + std::to_string(i));
  }

  EXPECT_TRUE(holder.IsFinished());
}

// =============================================================================
// ConcatenatingBatchHolder Tests
// =============================================================================

TEST(ConcatenatingBatchHolderTest, Construction) {
  ConcatenatingBatchHolder holder(1);

  EXPECT_EQ(holder.id(), 1U);
  EXPECT_EQ(holder.type(), BatchHolderType::kConcatenating);
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(ConcatenatingBatchHolderTest, SingleDataBlock) {
  ConcatenatingBatchHolder holder(1);

  (void)holder.Add(MakeTestData("data1", 256, 16));
  holder.Finish();

  ASSERT_OK_AND_ASSIGN(auto data, holder.Pull());
  EXPECT_GT(data->size_in_bytes(), 0U);  // Size depends on RecordBatch structure
}

TEST(ConcatenatingBatchHolderTest, SingleDataBlockPull) {
  // ConcatenatingBatchHolder with single block - returns as-is
  ConcatenatingBatchHolder holder(1);

  (void)holder.Add(MakeTestData("data1", 100, 10));

  EXPECT_EQ(holder.batch_count(), 1U);
  EXPECT_GT(holder.bytes_added(), 0U);  // Size depends on RecordBatch structure
  EXPECT_EQ(holder.rows_added(), 10U);

  holder.Finish();

  // Pull returns the single block
  ASSERT_OK_AND_ASSIGN(auto data, holder.Pull());
  EXPECT_EQ(data->num_rows(), 10U);

  EXPECT_EQ(holder.batch_count(), 0U);
  EXPECT_TRUE(holder.IsFinished());
}

TEST(ConcatenatingBatchHolderTest, TryPullBeforeFinish) {
  ConcatenatingBatchHolder holder(1);

  (void)holder.Add(MakeTestData("data1", 128, 8));

  // Try pull before finish should fail
  auto result = holder.TryPull();
  EXPECT_FALSE(result.has_value());

  holder.Finish();

  // Now it should work
  ASSERT_OK_AND_ASSIGN(auto data, holder.TryPull());
  EXPECT_GT(data->size_in_bytes(), 0U);  // Size depends on RecordBatch structure
  EXPECT_EQ(data->num_rows(), 8U);
}

// =============================================================================
// MakeBatchHolder Factory Tests
// =============================================================================

TEST(MakeBatchHolderTest, SimpleType) {
  auto holder = MakeBatchHolder(100, BatchHolderType::kSimple);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 100U);
  EXPECT_EQ(holder->type(), BatchHolderType::kSimple);
}

TEST(MakeBatchHolderTest, ConcatenatingType) {
  auto holder = MakeBatchHolder(101, BatchHolderType::kConcatenating);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 101U);
  EXPECT_EQ(holder->type(), BatchHolderType::kConcatenating);
}

TEST(MakeBatchHolderTest, WithConfig) {
  BatchHolderConfig config;
  config.flow_control_batches_threshold = 10;
  config.flow_control_bytes_threshold = 1024 * 1024;
  config.auto_spill = false;

  auto holder = MakeBatchHolder(102, BatchHolderType::kSimple, config);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 102U);
}

// =============================================================================
// Thread Safety Tests
// =============================================================================

TEST(BatchHolderThreadTest, ProducerConsumer) {
  BatchHolder holder(1);

  // Producer
  auto producer = std::async(std::launch::async, [&holder]() {
    for (int i = 0; i < 10; ++i) {
      (void)holder.Add(MakeTestData("data" + std::to_string(i), 64, 4));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    holder.Finish();
  });

  // Consumer
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

TEST(BatchHolderThreadTest, ConcurrentAdds) {
  BatchHolder holder(1);

  constexpr int kNumThreads = 4;
  constexpr int kBlocksPerThread = 25;

  std::vector<std::future<void>> futures;
  for (int t = 0; t < kNumThreads; ++t) {
    futures.push_back(std::async(std::launch::async, [&holder, t]() {
      for (int i = 0; i < kBlocksPerThread; ++i) {
        auto block_id = std::to_string(t) + "_" + std::to_string(i);
        (void)holder.Add(MakeTestData(block_id, 32, 2));
      }
    }));
  }

  for (auto& fut : futures) {
    fut.get();
  }

  EXPECT_EQ(holder.batch_count(),
            static_cast<size_t>(kNumThreads * kBlocksPerThread));

  holder.Finish();

  // Consume all
  int count = 0;
  while (holder.HasNext()) {
    auto result = holder.TryPull();
    if (result.has_value()) {
      ++count;
    }
  }
  EXPECT_EQ(count, kNumThreads * kBlocksPerThread);
}

// =============================================================================
// IBatchHolder Interface Tests
// =============================================================================

TEST(IBatchHolderTest, PolymorphicAccess) {
  std::shared_ptr<IBatchHolder> holder =
      MakeBatchHolder(1, BatchHolderType::kSimple);

  EXPECT_EQ(holder->id(), 1U);
  EXPECT_EQ(holder->type(), BatchHolderType::kSimple);
  EXPECT_FALSE(holder->IsFinished());

  holder->Finish();
  EXPECT_TRUE(holder->IsFinished());
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(BatchHolderEdgeCaseTest, EmptyHolder) {
  BatchHolder holder(1);
  holder.Finish();

  EXPECT_TRUE(holder.IsFinished());
  EXPECT_FALSE(holder.HasNext());
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderEdgeCaseTest, ZeroSizeDataBlock) {
  BatchHolder holder(1);

  auto data = MakeTestData("empty", 0, 0);

  auto status = holder.Add(std::move(data));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(holder.bytes_added(), 0U);
  EXPECT_EQ(holder.rows_added(), 0U);
}

TEST(BatchHolderEdgeCaseTest, LargeBatch) {
  BatchHolder holder(1);

  // Large data block with many rows
  auto data = MakeTestData("large", 0, 10000);
  auto status = holder.Add(std::move(data));

  EXPECT_TRUE(status.ok());
  EXPECT_GT(holder.bytes_added(), 0U);  // Size depends on RecordBatch structure
  EXPECT_EQ(holder.rows_added(), 10000U);
}
