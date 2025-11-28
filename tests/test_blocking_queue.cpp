/// @file test_blocking_queue.cpp
/// @brief Tests for the BlockingQueue implementation

#include <quest/cache/blocking_queue.hpp>
#include <quest/cache/cache_data.hpp>
#include <cassert>
#include <iostream>
#include <thread>
#include <chrono>

using namespace quest;
using namespace quest::cache;

// Test message type
struct TestMessage {
    std::string id_;
    int value;
    
    [[nodiscard]] std::string_view id() const noexcept { return id_; }
};

using TestMessagePtr = std::unique_ptr<TestMessage>;

void test_basic_put_pop() {
    std::cout << "test_basic_put_pop... ";
    
    BlockingQueue<TestMessagePtr> queue;
    
    // Put a message
    auto msg = std::make_unique<TestMessage>("test_1", 42);
    auto status = queue.Put(std::move(msg));
    assert(status.ok());
    assert(queue.Size() == 1);
    
    // Pop the message
    auto result = queue.Pop();
    assert(result.has_value());
    assert(result.value()->id_ == "test_1");
    assert(result.value()->value == 42);
    assert(queue.Empty());
    
    std::cout << "PASSED\n";
}

void test_try_pop_empty() {
    std::cout << "test_try_pop_empty... ";
    
    BlockingQueue<TestMessagePtr> queue;
    
    auto result = queue.TryPop();
    assert(!result.has_value());
    assert(result.error().code() == ErrorCode::Invalid);
    
    std::cout << "PASSED\n";
}

void test_finish() {
    std::cout << "test_finish... ";
    
    BlockingQueue<TestMessagePtr> queue;
    
    queue.Put(std::make_unique<TestMessage>("1", 1));
    queue.Finish();
    
    // Can still pop existing items
    auto result = queue.Pop();
    assert(result.has_value());
    
    // But next pop returns error
    result = queue.Pop();
    assert(!result.has_value());
    assert(result.error().code() == ErrorCode::Invalid);
    
    // Can't add after finish
    auto status = queue.Put(std::make_unique<TestMessage>("2", 2));
    assert(!status.ok());
    
    std::cout << "PASSED\n";
}

void test_pop_by_id() {
    std::cout << "test_pop_by_id... ";
    
    BlockingQueue<TestMessagePtr> queue;
    
    queue.Put(std::make_unique<TestMessage>("first", 1));
    queue.Put(std::make_unique<TestMessage>("second", 2));
    queue.Put(std::make_unique<TestMessage>("third", 3));
    
    // Pop the middle one by ID
    auto result = queue.PopById("second");
    assert(result.has_value());
    assert(result.value()->value == 2);
    assert(queue.Size() == 2);
    
    std::cout << "PASSED\n";
}

void test_multithreaded() {
    std::cout << "test_multithreaded... ";
    
    BlockingQueue<TestMessagePtr> queue;
    constexpr int num_messages = 100;
    
    // Producer thread
    std::thread producer([&queue]() {
        for (int i = 0; i < num_messages; ++i) {
            queue.Put(std::make_unique<TestMessage>(
                std::to_string(i), i));
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        }
        queue.Finish();
    });
    
    // Consumer thread
    int received = 0;
    std::thread consumer([&queue, &received]() {
        while (true) {
            auto result = queue.Pop();
            if (!result.has_value()) break;
            ++received;
        }
    });
    
    producer.join();
    consumer.join();
    
    assert(received == num_messages);
    
    std::cout << "PASSED\n";
}

void test_timeout() {
    std::cout << "test_timeout... ";
    
    BlockingQueue<TestMessagePtr> queue(QueueConfig{
        .timeout = std::chrono::milliseconds(50)
    });
    
    auto start = std::chrono::steady_clock::now();
    auto result = queue.Pop();
    auto elapsed = std::chrono::steady_clock::now() - start;
    
    assert(!result.has_value());
    assert(elapsed >= std::chrono::milliseconds(40));  // Allow some margin
    
    std::cout << "PASSED\n";
}

int main() {
    std::cout << "=== BlockingQueue Tests ===\n";
    
    test_basic_put_pop();
    test_try_pop_empty();
    test_finish();
    test_pop_by_id();
    test_multithreaded();
    test_timeout();
    
    std::cout << "\nAll tests passed!\n";
    return 0;
}
