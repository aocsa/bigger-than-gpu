// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_GENERATOR_HPP
#define QUEST_GENERATOR_HPP

/// @file generator.hpp
/// @brief C++20 coroutine generator for lazy, pull-based iteration.

#include <coroutine>
#include <exception>
#include <optional>
#include <utility>

#include "quest/datasource/record_batch.hpp"
#include "quest/result.hpp"

namespace quest {

// =============================================================================
// Generator Coroutine
// =============================================================================

/// @brief A lazy generator for streaming values with backpressure.
///
/// Yields values on-demand, allowing the consumer to control the pace of
/// execution. Memory is only allocated when the consumer requests the next
/// value.
///
/// @tparam T The type of values yielded.
template <typename T>
class Generator {
 public:
  struct promise_type;
  using handle_type = std::coroutine_handle<promise_type>;

  struct promise_type {
    std::optional<T> current_value;
    std::exception_ptr exception;

    Generator get_return_object() {
      return Generator{handle_type::from_promise(*this)};
    }

    std::suspend_always initial_suspend() noexcept { return {}; }
    std::suspend_always final_suspend() noexcept { return {}; }

    std::suspend_always yield_value(T value) noexcept {
      current_value = std::move(value);
      return {};
    }

    void return_void() noexcept {}

    void unhandled_exception() noexcept {
      exception = std::current_exception();
    }
  };

  // Rule of Five: Move-only type
  Generator(Generator&& other) noexcept : handle_(std::exchange(other.handle_, {})) {}

  Generator& operator=(Generator&& other) noexcept {
    if (this != &other) {
      if (handle_) {
        handle_.destroy();
      }
      handle_ = std::exchange(other.handle_, {});
    }
    return *this;
  }

  Generator(const Generator&) = delete;
  Generator& operator=(const Generator&) = delete;

  ~Generator() {
    if (handle_) {
      handle_.destroy();
    }
  }

  /// @brief Iterator for range-based for loop support.
  class iterator {
   public:
    using iterator_category = std::input_iterator_tag;
    using value_type = T;
    using difference_type = std::ptrdiff_t;
    using pointer = T*;
    using reference = T&;

    iterator() noexcept = default;
    explicit iterator(handle_type handle) noexcept : handle_(handle) {}

    iterator& operator++() {
      if (handle_ && !handle_.done()) {
        handle_.resume();
        if (handle_.done()) {
          handle_ = {};
        }
      }
      return *this;
    }

    iterator operator++(int) {
      iterator tmp = *this;
      ++(*this);
      return tmp;
    }

    [[nodiscard]] T operator*() const {
      if (handle_.promise().exception) {
        std::rethrow_exception(handle_.promise().exception);
      }
      return std::move(*handle_.promise().current_value);
    }

    [[nodiscard]] bool operator==(const iterator& other) const noexcept {
      return handle_ == other.handle_ ||
             ((!handle_ || handle_.done()) && (!other.handle_ || other.handle_.done()));
    }

    [[nodiscard]] bool operator!=(const iterator& other) const noexcept {
      return !(*this == other);
    }

   private:
    handle_type handle_{};
  };

  [[nodiscard]] iterator begin() {
    if (handle_ && !handle_.done()) {
      handle_.resume();
      if (handle_.done()) {
        return iterator{};
      }
    }
    return iterator{handle_};
  }

  [[nodiscard]] iterator end() noexcept { return iterator{}; }

  [[nodiscard]] bool has_next() const noexcept {
    return handle_ && !handle_.done();
  }

  [[nodiscard]] std::optional<T> next() {
    if (!handle_ || handle_.done()) {
      return std::nullopt;
    }
    handle_.resume();
    if (handle_.done()) {
      return std::nullopt;
    }
    if (handle_.promise().exception) {
      std::rethrow_exception(handle_.promise().exception);
    }
    return std::move(handle_.promise().current_value);
  }

 private:
  explicit Generator(handle_type handle) noexcept : handle_(handle) {}
  handle_type handle_{};
};

/// Batch generator alias — pull-based, consumer-paced.
using BatchGenerator = Generator<Result<datasource::RecordBatch>>;

}  // namespace quest

#endif  // QUEST_GENERATOR_HPP
