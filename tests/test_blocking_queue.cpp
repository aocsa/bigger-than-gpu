/// @file test_blocking_queue.cpp
/// @brief Tests for the BlockingQueue implementation

#include <gtest/gtest.h>

#include <quest/cache/blocking_queue.hpp>
#include <quest/cache/data_block.hpp>

#include <chrono>
#include <thread>

using namespace quest;
using namespace quest::cache;

struct TestMessage {
  std::string id_;
  int value;

  [[nodiscard]] std::string_view id() const noexcept { return id_; }
};

using TestMessagePtr = std::unique_ptr<TestMessage>;

class BlockingQueueTest : public ::testing::Test {
 protected:
  BlockingQueue<TestMessagePtr> queue_;
};

TEST_F(BlockingQueueTest, BasicPutPop) {
  auto msg = std::make_unique<TestMessage>("test_1", 42);
  auto status = queue_.Put(std::move(msg));
  ASSERT_TRUE(status.ok());
  ASSERT_EQ(queue_.Size(), 1);

  auto result = queue_.Pop();
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->id_, "test_1");
  EXPECT_EQ(result.value()->value, 42);
  EXPECT_TRUE(queue_.Empty());
}

TEST_F(BlockingQueueTest, TryPopEmpty) {
  auto result = queue_.TryPop();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Invalid);
}

TEST_F(BlockingQueueTest, Finish) {
  (void)queue_.Put(std::make_unique<TestMessage>("1", 1));
  queue_.Finish();

  // Can still pop existing items
  auto result = queue_.Pop();
  ASSERT_TRUE(result.has_value());

  result = queue_.Pop();
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::Invalid);

  auto status = queue_.Put(std::make_unique<TestMessage>("2", 2));
  EXPECT_FALSE(status.ok());
}

TEST_F(BlockingQueueTest, PopById) {
  (void)queue_.Put(std::make_unique<TestMessage>("first", 1));
  (void)queue_.Put(std::make_unique<TestMessage>("second", 2));
  (void)queue_.Put(std::make_unique<TestMessage>("third", 3));

  auto result = queue_.PopById("second");
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result.value()->value, 2);
  EXPECT_EQ(queue_.Size(), 2);
}

TEST_F(BlockingQueueTest, Multithreaded) {
  constexpr int num_messages = 100;

  std::thread producer([this]() {
    for (int i = 0; i < num_messages; ++i) {
      (void)queue_.Put(std::make_unique<TestMessage>(std::to_string(i), i));
      std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    queue_.Finish();
  });

  int received = 0;
  std::thread consumer([this, &received]() {
    while (true) {
      auto result = queue_.Pop();
      if (!result.has_value()) break;
      ++received;
    }
  });

  producer.join();
  consumer.join();

  EXPECT_EQ(received, num_messages);
}

TEST(BlockingQueueTimeoutTest, PopTimeout) {
  BlockingQueue<TestMessagePtr> queue(
      QueueConfig{.timeout = std::chrono::milliseconds(50)});

  auto start = std::chrono::steady_clock::now();
  auto result = queue.Pop();
  auto elapsed = std::chrono::steady_clock::now() - start;

  ASSERT_FALSE(result.has_value());
  EXPECT_GE(elapsed, std::chrono::milliseconds(40));  // Allow some margin
}
