#pragma once

/// @file blocking_queue.hpp
/// @brief Thread-safe blocking queue for message passing between operators

#include <algorithm>
#include <chrono>
#include <concepts>
#include <condition_variable>
#include <deque>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quest/result.hpp"

namespace quest::cache {

using quest::Error;
using quest::ErrorCode;
using quest::Result;
using quest::Status;

// =============================================================================
// Concept: Message must have an id() method returning string_view
// =============================================================================

template <typename T>
concept HasId = requires(const T& msg) {
  { msg.id() } -> std::convertible_to<std::string_view>;
};

// =============================================================================
// Queue Configuration
// =============================================================================

struct QueueConfig {
  std::chrono::milliseconds timeout{0};  // 0 means no timeout (wait forever)
};

// =============================================================================
// BlockingQueue Implementation
// =============================================================================

template <typename T>
  requires HasId<std::remove_pointer_t<typename T::element_type>>
class BlockingQueue {
 public:
  explicit BlockingQueue(QueueConfig config = {}) : config_(config) {}

  ~BlockingQueue() { Finish(); }

  // Non-copyable, non-movable for thread safety
  BlockingQueue(const BlockingQueue&) = delete;
  BlockingQueue& operator=(const BlockingQueue&) = delete;
  BlockingQueue(BlockingQueue&&) = delete;
  BlockingQueue& operator=(BlockingQueue&&) = delete;

  // ===========================================================================
  // Producer Operations
  // ===========================================================================

  [[nodiscard]] Status Put(T item) {
    if (!item) {
      return Status::Invalid("Cannot enqueue null message");
    }

    {
      std::lock_guard lock(mutex_);

      if (finished_) {
        return Status::Invalid("Cannot enqueue to finished queue");
      }

      queue_.push_back(std::move(item));
      ++batches_count_;
    }

    not_empty_.notify_one();
    return Status::OK();
  }

  void Finish() {
    {
      std::lock_guard lock(mutex_);
      if (finished_) {
        return;
      }
      finished_ = true;
    }
    not_empty_.notify_all();
  }

  // ===========================================================================
  // Consumer Operations
  // ===========================================================================

  /// @brief Blocks until an item is available or the queue is finished.
  /// @return The front item, or std::nullopt once the queue is finished and
  ///         empty. Timeouts are errors.
  [[nodiscard]] Result<std::optional<T>> Pop() {
    std::unique_lock lock(mutex_);

    if (!WaitFor(lock, [this] { return !queue_.empty() || finished_; })) {
      return quest::Status::IOError("Pop operation timed out");
    }

    if (queue_.empty()) {
      return std::nullopt;
    }

    return PopFront();
  }

  /// @brief Non-blocking pop.
  /// @return The front item, or std::nullopt if the queue is currently empty
  ///         (finished or not; use IsFinished() to tell them apart).
  [[nodiscard]] Result<std::optional<T>> TryPop() {
    std::lock_guard lock(mutex_);

    if (queue_.empty()) {
      return std::nullopt;
    }

    return PopFront();
  }

  [[nodiscard]] Result<std::vector<T>> PopAll() {
    std::unique_lock lock(mutex_);

    if (!WaitFor(lock, [this] { return !queue_.empty() || finished_; })) {
      return Status::IOError("PopAll operation timed out");
    }

    std::vector<T> result;
    result.reserve(queue_.size());

    while (!queue_.empty()) {
      result.push_back(std::move(queue_.front()));
      queue_.pop_front();
    }
    batches_count_ = 0;

    return result;
  }

  // ===========================================================================
  // ID-based Operations
  // ===========================================================================

  [[nodiscard]] Result<T> PopById(std::string_view msg_id) {
    std::unique_lock lock(mutex_);

    if (!WaitFor(lock,
                 [this, msg_id] { return ContainsId(msg_id) || finished_; })) {
      return Status::IOError(
          std::format("PopById timed out waiting for id: {}", msg_id));
    }

    auto iter = std::ranges::find_if(
        queue_, [msg_id](const auto& item) { return item->id() == msg_id; });

    if (iter == queue_.end()) {
      return Status::NotFound(
          std::format("Message with id '{}' not found", msg_id));
    }

    auto item = std::move(*iter);
    queue_.erase(iter);
    --batches_count_;
    return item;
  }

  // ===========================================================================
  // Status Queries
  // ===========================================================================

  [[nodiscard]] bool IsFinished() const {
    std::lock_guard lock(mutex_);
    return finished_;
  }

  [[nodiscard]] bool Empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
  }

  [[nodiscard]] std::size_t Size() const {
    std::lock_guard lock(mutex_);
    return queue_.size();
  }

  [[nodiscard]] bool HasNext() const {
    std::lock_guard lock(mutex_);
    return !queue_.empty();
  }

  [[nodiscard]] Status WaitUntilFinished() {
    std::unique_lock lock(mutex_);

    if (!WaitFor(lock, [this] { return finished_; })) {
      return Status::IOError("WaitUntilFinished timed out");
    }

    return Status::OK();
  }

  [[nodiscard]] const QueueConfig& Config() const { return config_; }

  [[nodiscard]] std::size_t BatchesCount() const {
    std::lock_guard lock(mutex_);
    return batches_count_;
  }

 private:
  // Helper: Pop front element (caller must hold lock and ensure queue
  // non-empty)
  T PopFront() {
    auto item = std::move(queue_.front());
    queue_.pop_front();
    --batches_count_;
    return item;
  }

  [[nodiscard]] bool ContainsId(std::string_view msg_id) const {
    return std::ranges::any_of(
        queue_, [msg_id](const auto& item) { return item->id() == msg_id; });
  }

  template <typename Predicate>
  bool WaitFor(std::unique_lock<std::mutex>& lock, Predicate predicate) {
    if (config_.timeout.count() > 0) {
      return not_empty_.wait_for(lock, config_.timeout, predicate);
    }
    not_empty_.wait(lock, predicate);
    return true;
  }

  mutable std::mutex mutex_;
  std::condition_variable not_empty_;
  std::deque<T> queue_;
  bool finished_{false};
  std::size_t batches_count_{0};
  QueueConfig config_;
};

}  // namespace quest::cache
