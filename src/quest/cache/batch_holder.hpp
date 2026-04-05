#pragma once

/// @file batch_holder.hpp
/// @brief BatchHolder - the cache machine for holding data batches between
/// operators
/// @details Renamed from CacheMachine. Manages data flow between operators
/// using a
///          blocking queue and handles spilling to disk when CPU memory is
///          exhausted.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "quest/cache/blocking_queue.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"

namespace quest::cache {

// =============================================================================
// BatchHolder Configuration
// =============================================================================

/// @brief Configuration for BatchHolder
struct BatchHolderConfig {
  std::size_t flow_control_batches_threshold{
      std::numeric_limits<std::size_t>::max()};
  std::size_t flow_control_bytes_threshold{
      std::numeric_limits<std::size_t>::max()};
  std::chrono::milliseconds timeout{0};  // 0 = no timeout
  bool auto_spill{true};  // Automatically spill to disk when CPU is saturated
};

/// @brief Type of batch holder (controls dequeue behavior)
enum class BatchHolderType : std::uint8_t {
  kSimple,        ///< Each batch is pulled individually
  kConcatenating  ///< All batches are concatenated into a single result
};

// =============================================================================
// BatchHolder Interface
// =============================================================================

/// @brief Abstract interface for batch holders
class IBatchHolder {
 public:
  virtual ~IBatchHolder() = default;

  /// @brief Get unique identifier for this batch holder
  [[nodiscard]] virtual std::uint64_t id() const noexcept = 0;

  /// @brief Get the batch holder type
  [[nodiscard]] virtual BatchHolderType type() const noexcept = 0;

  /// @brief Add a data block to the holder
  [[nodiscard]] virtual Status Add(DataBlockPtr data) = 0;

  /// @brief Retrieve the next batch (blocks if empty)
  [[nodiscard]] virtual Result<DataBlockPtr> Pull() = 0;

  /// @brief Try to retrieve without blocking
  [[nodiscard]] virtual Result<DataBlockPtr> TryPull() = 0;

  /// @brief Mark the holder as finished (no more data will be added)
  virtual void Finish() = 0;

  /// @brief Check if the holder is finished AND empty
  [[nodiscard]] virtual bool IsFinished() const = 0;

  /// @brief Check if there are batches available
  [[nodiscard]] virtual bool HasNext() const = 0;

  /// @brief Get the number of batches currently in the holder
  [[nodiscard]] virtual std::size_t batch_count() const = 0;

  /// @brief Get the total bytes added to this holder
  [[nodiscard]] virtual std::size_t bytes_added() const noexcept = 0;

  /// @brief Get the total rows added to this holder
  [[nodiscard]] virtual std::size_t rows_added() const noexcept = 0;

  /// @brief Wait until the holder is not saturated (for flow control)
  virtual void WaitIfSaturated() = 0;
};

// =============================================================================
// BatchHolderBase - Common implementation
// =============================================================================

/// @brief Base class providing common batch holder functionality
class BatchHolderBase : public IBatchHolder {
 public:
  BatchHolderBase(
      std::uint64_t id, BatchHolderConfig config,
      std::shared_ptr<memory::MemoryResourceManager> memory_manager);

  ~BatchHolderBase() override = default;

  // Non-copyable, non-movable
  BatchHolderBase(const BatchHolderBase&) = delete;
  BatchHolderBase& operator=(const BatchHolderBase&) = delete;
  BatchHolderBase(BatchHolderBase&&) = delete;
  BatchHolderBase& operator=(BatchHolderBase&&) = delete;

  // Common IBatchHolder implementations
  [[nodiscard]] std::uint64_t id() const noexcept override { return id_; }
  void Finish() override;
  [[nodiscard]] bool IsFinished() const override;
  [[nodiscard]] std::size_t batch_count() const override;
  [[nodiscard]] std::size_t bytes_added() const noexcept override {
    return bytes_added_.load();
  }
  [[nodiscard]] std::size_t rows_added() const noexcept override {
    return rows_added_.load();
  }

 protected:
  /// @brief Notify threads waiting on saturation condition
  void NotifySaturationWaiters();

  /// @brief Track bytes and rows added
  void TrackAdded(std::size_t bytes, std::size_t rows);

  std::uint64_t id_;
  BatchHolderConfig config_;
  BlockingQueue<DataBlockPtr> queue_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  std::atomic<std::size_t> bytes_added_{0};
  std::atomic<std::size_t> rows_added_{0};

  mutable std::mutex saturation_mutex_;
  std::condition_variable saturation_cv_;
};

// =============================================================================
// BatchHolder Implementation
// =============================================================================

/// @brief Standard batch holder implementation
class BatchHolder final : public BatchHolderBase {
 public:
  BatchHolder(
      std::uint64_t id, BatchHolderConfig config = {},
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  ~BatchHolder() override = default;

  // IBatchHolder interface
  [[nodiscard]] BatchHolderType type() const noexcept override {
    return BatchHolderType::kSimple;
  }

  [[nodiscard]] Status Add(DataBlockPtr data) override;
  [[nodiscard]] Result<DataBlockPtr> Pull() override;
  [[nodiscard]] Result<DataBlockPtr> TryPull() override;
  [[nodiscard]] bool HasNext() const override;
  void WaitIfSaturated() override;

 private:
  /// @brief Check if we should spill data to disk
  [[nodiscard]] bool ShouldSpill() const;

  /// @brief Spill a data block to disk if needed
  [[nodiscard]] Status MaybeSpill(DataBlockPtr& data);

  std::atomic<std::size_t> current_bytes_{0};
};

// =============================================================================
// ConcatenatingBatchHolder Implementation
// =============================================================================

/// @brief Batch holder that concatenates all batches before returning
class ConcatenatingBatchHolder final : public BatchHolderBase {
 public:
  ConcatenatingBatchHolder(
      std::uint64_t id, BatchHolderConfig config = {},
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  ~ConcatenatingBatchHolder() override = default;

  // IBatchHolder interface
  [[nodiscard]] BatchHolderType type() const noexcept override {
    return BatchHolderType::kConcatenating;
  }

  [[nodiscard]] Status Add(DataBlockPtr data) override;
  [[nodiscard]] Result<DataBlockPtr> Pull() override;
  [[nodiscard]] Result<DataBlockPtr> TryPull() override;
  [[nodiscard]] bool HasNext() const override;
  void WaitIfSaturated() override;

 private:
  /// @brief Concatenate all pending batches into one
  [[nodiscard]] Result<DataBlockPtr> ConcatenateAll();
};

// =============================================================================
// Factory Function
// =============================================================================

/// @brief Create a batch holder of the specified type
[[nodiscard]] std::shared_ptr<IBatchHolder> MakeBatchHolder(
    std::uint64_t id, BatchHolderType type, BatchHolderConfig config = {},
    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

}  // namespace quest::cache
