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
#include "quest/cache/cache_data.hpp"
#include "quest/tests/utils.hpp"

using namespace quest;
using namespace quest::cache;
using namespace quest::memory;

// =============================================================================
// Helper Functions
// =============================================================================

namespace {

std::shared_ptr<CacheData> MakeTestData(const std::string& id, size_t bytes,
                                        size_t rows) {
  std::vector<std::byte> data(bytes);
  return std::make_shared<CPUCacheData>(id, std::move(data), rows, 1);
}

CacheMessagePtr MakeTestMessage(const std::string& id, size_t bytes,
                                size_t rows) {
  auto data = MakeTestData(id, bytes, rows);
  return std::make_unique<CacheMessage>(id, data);
}

}  // namespace

// =============================================================================
// BatchHolder Basic Tests
// =============================================================================

TEST(BatchHolderTest, Construction) {
  BatchHolder holder(1);

  EXPECT_EQ(holder.id(), 1U);
  EXPECT_EQ(holder.type(), BatchHolderType::Simple);
  EXPECT_EQ(holder.batch_count(), 0U);
  EXPECT_EQ(holder.bytes_added(), 0U);
  EXPECT_EQ(holder.rows_added(), 0U);
  EXPECT_FALSE(holder.has_next());
  EXPECT_FALSE(holder.is_finished());
}

TEST(BatchHolderTest, AddMessage) {
  BatchHolder holder(1);

  auto msg = MakeTestMessage("msg1", 1024, 100);
  auto status = holder.add(std::move(msg));

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(holder.batch_count(), 1U);
  EXPECT_EQ(holder.bytes_added(), 1024U);
  EXPECT_EQ(holder.rows_added(), 100U);
  EXPECT_TRUE(holder.has_next());
}

TEST(BatchHolderTest, AddWithIdAndData) {
  BatchHolder holder(1);

  auto data = MakeTestData("data1", 512, 50);
  auto status = holder.add("msg1", data);

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(holder.batch_count(), 1U);
  EXPECT_EQ(holder.bytes_added(), 512U);
  EXPECT_EQ(holder.rows_added(), 50U);
}

TEST(BatchHolderTest, AddNullMessage) {
  BatchHolder holder(1);

  auto status = holder.add(nullptr);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderTest, TryPull) {
  BatchHolder holder(1);

  // Empty queue
  auto empty_result = holder.try_pull();
  EXPECT_FALSE(empty_result.has_value());

  // Add message
  (void)holder.add(MakeTestMessage("msg1", 256, 10));

  // Pull it
  ASSERT_OK_AND_ASSIGN(auto msg, holder.try_pull());
  EXPECT_EQ(msg->id(), "msg1");
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderTest, PullAfterFinish) {
  BatchHolder holder(1);

  (void)holder.add(MakeTestMessage("msg1", 256, 10));
  holder.finish();

  ASSERT_OK_AND_ASSIGN(auto msg, holder.pull());
  EXPECT_EQ(msg->id(), "msg1");
}

TEST(BatchHolderTest, IsFinished) {
  BatchHolder holder(1);

  EXPECT_FALSE(holder.is_finished());

  (void)holder.add(MakeTestMessage("msg1", 100, 5));
  EXPECT_FALSE(holder.is_finished());

  holder.finish();
  // Still has messages
  EXPECT_FALSE(holder.is_finished());

  // Pull all messages
  (void)holder.pull();
  // Now finished and empty
  EXPECT_TRUE(holder.is_finished());
}

TEST(BatchHolderTest, MultipleBatches) {
  BatchHolder holder(1);

  for (int i = 0; i < 5; ++i) {
    (void)holder.add(MakeTestMessage("msg" + std::to_string(i), 100, 10));
  }

  EXPECT_EQ(holder.batch_count(), 5U);
  EXPECT_EQ(holder.bytes_added(), 500U);
  EXPECT_EQ(holder.rows_added(), 50U);

  holder.finish();

  // Pull in order (FIFO)
  for (int i = 0; i < 5; ++i) {
    ASSERT_OK_AND_ASSIGN(auto msg, holder.pull());
    EXPECT_EQ(msg->id(), "msg" + std::to_string(i));
  }

  EXPECT_TRUE(holder.is_finished());
}

// =============================================================================
// ConcatenatingBatchHolder Tests
// =============================================================================

TEST(ConcatenatingBatchHolderTest, Construction) {
  ConcatenatingBatchHolder holder(1);

  EXPECT_EQ(holder.id(), 1U);
  EXPECT_EQ(holder.type(), BatchHolderType::Concatenating);
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(ConcatenatingBatchHolderTest, SingleMessage) {
  ConcatenatingBatchHolder holder(1);

  (void)holder.add(MakeTestMessage("msg1", 256, 16));
  holder.finish();

  ASSERT_OK_AND_ASSIGN(auto msg, holder.pull());
  EXPECT_EQ(msg->size_in_bytes(), 256U);
}

TEST(ConcatenatingBatchHolderTest, MultipleMessages) {
  ConcatenatingBatchHolder holder(1);

  (void)holder.add(MakeTestMessage("msg1", 100, 10));
  (void)holder.add(MakeTestMessage("msg2", 100, 10));
  (void)holder.add(MakeTestMessage("msg3", 100, 10));

  EXPECT_EQ(holder.batch_count(), 3U);
  EXPECT_EQ(holder.bytes_added(), 300U);
  EXPECT_EQ(holder.rows_added(), 30U);

  holder.finish();

  // Pull returns concatenated result
  ASSERT_OK_AND_ASSIGN(auto msg, holder.pull());
  EXPECT_EQ(msg->size_in_bytes(), 300U);

  EXPECT_EQ(holder.batch_count(), 0U);
  EXPECT_TRUE(holder.is_finished());
}

TEST(ConcatenatingBatchHolderTest, TryPullBeforeFinish) {
  ConcatenatingBatchHolder holder(1);

  (void)holder.add(MakeTestMessage("msg1", 128, 8));

  // Try pull before finish should fail
  auto result = holder.try_pull();
  EXPECT_FALSE(result.has_value());

  holder.finish();

  // Now it should work
  ASSERT_OK_AND_ASSIGN(auto msg, holder.try_pull());
  EXPECT_EQ(msg->size_in_bytes(), 128U);
}

// =============================================================================
// make_batch_holder Factory Tests
// =============================================================================

TEST(MakeBatchHolderTest, SimpleType) {
  auto holder = make_batch_holder(100, BatchHolderType::Simple);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 100U);
  EXPECT_EQ(holder->type(), BatchHolderType::Simple);
}

TEST(MakeBatchHolderTest, ConcatenatingType) {
  auto holder = make_batch_holder(101, BatchHolderType::Concatenating);

  ASSERT_NE(holder, nullptr);
  EXPECT_EQ(holder->id(), 101U);
  EXPECT_EQ(holder->type(), BatchHolderType::Concatenating);
}

TEST(MakeBatchHolderTest, WithConfig) {
  BatchHolderConfig config;
  config.flow_control_batches_threshold = 10;
  config.flow_control_bytes_threshold = 1024 * 1024;
  config.auto_spill = false;

  auto holder = make_batch_holder(102, BatchHolderType::Simple, config);

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
      (void)holder.add(MakeTestMessage("msg" + std::to_string(i), 64, 4));
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    holder.finish();
  });

  // Consumer
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

TEST(BatchHolderThreadTest, ConcurrentAdds) {
  BatchHolder holder(1);

  constexpr int kNumThreads = 4;
  constexpr int kMessagesPerThread = 25;

  std::vector<std::future<void>> futures;
  for (int t = 0; t < kNumThreads; ++t) {
    futures.push_back(std::async(std::launch::async, [&holder, t]() {
      for (int i = 0; i < kMessagesPerThread; ++i) {
        auto id = std::to_string(t) + "_" + std::to_string(i);
        (void)holder.add(MakeTestMessage(id, 32, 2));
      }
    }));
  }

  for (auto& f : futures) {
    f.get();
  }

  EXPECT_EQ(holder.batch_count(),
            static_cast<size_t>(kNumThreads * kMessagesPerThread));

  holder.finish();

  // Consume all
  int count = 0;
  while (holder.has_next()) {
    auto result = holder.try_pull();
    if (result.has_value()) {
      ++count;
    }
  }
  EXPECT_EQ(count, kNumThreads * kMessagesPerThread);
}

// =============================================================================
// IBatchHolder Interface Tests
// =============================================================================

TEST(IBatchHolderTest, PolymorphicAccess) {
  std::shared_ptr<IBatchHolder> holder =
      make_batch_holder(1, BatchHolderType::Simple);

  EXPECT_EQ(holder->id(), 1U);
  EXPECT_EQ(holder->type(), BatchHolderType::Simple);
  EXPECT_FALSE(holder->is_finished());

  holder->finish();
  EXPECT_TRUE(holder->is_finished());
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(BatchHolderEdgeCaseTest, EmptyHolder) {
  BatchHolder holder(1);
  holder.finish();

  EXPECT_TRUE(holder.is_finished());
  EXPECT_FALSE(holder.has_next());
  EXPECT_EQ(holder.batch_count(), 0U);
}

TEST(BatchHolderEdgeCaseTest, ZeroSizeMessage) {
  BatchHolder holder(1);

  auto data = MakeTestData("empty", 0, 0);
  auto msg = std::make_unique<CacheMessage>("empty", data);

  auto status = holder.add(std::move(msg));
  EXPECT_TRUE(status.ok());
  EXPECT_EQ(holder.bytes_added(), 0U);
  EXPECT_EQ(holder.rows_added(), 0U);
}

TEST(BatchHolderEdgeCaseTest, LargeBatch) {
  BatchHolder holder(1);

  // 1 MB message
  auto msg = MakeTestMessage("large", 1024 * 1024, 10000);
  auto status = holder.add(std::move(msg));

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(holder.bytes_added(), 1024U * 1024U);
}
