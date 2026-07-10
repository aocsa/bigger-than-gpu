#ifndef QUEST_CACHE_BATCH_HOLDER_HPP_
#define QUEST_CACHE_BATCH_HOLDER_HPP_

/// @file batch_holder.hpp
/// @brief BatchHolder - the cache machine for holding data batches between
/// operators
/// @details Renamed from CacheMachine. Manages data flow between operators
/// using a
///          blocking queue and handles spilling to disk when CPU memory is
///          exhausted.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
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
  /// Concatenating holders only: merge pending blocks into chunks of up to
  /// this many bytes per Pull instead of one all-encompassing block.
  /// 0 = concatenate everything (classic behavior). BlazingSQL heritage:
  /// the mature engine's concat byte cap (Sirius: _concat_batch_bytes).
  std::size_t concat_bytes_threshold{0};
};

/// @brief Type of batch holder (controls dequeue behavior)
enum class BatchHolderType : std::uint8_t {
  kSimple,         ///< Each batch is pulled individually
  kConcatenating,  ///< Batches are concatenated (all, or in byte-capped chunks)
  kNonWaiting      ///< Non-blocking pull; pre-filled single-shot handoffs
                   ///< (BlazingSQL NonWaitingCacheMachine). Empty == drained.
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

  /// @brief Live-update flow-control thresholds and wake blocked producers.
  ///
  /// Today this unblocks threads in WaitIfSaturated() via NotifySaturationWaiters.
  /// Phase 3: after Notify, also post a task-re-evaluation event to the creator
  /// so WAITING producers are re-armed without an unrelated completion.
  virtual void SetFlowControl(std::size_t max_batches, std::size_t max_bytes) = 0;

  /// @brief Live-update spill eligibility. Never → auto_spill=false;
  ///        Eligible/First → auto_spill=true (phase 5 will use richer semantics).
  virtual void SetSpillPolicy(bool auto_spill) = 0;

  /// @brief Current batch-count flow-control threshold (live-readable).
  [[nodiscard]] virtual std::size_t flow_control_batches_threshold() const noexcept = 0;

  /// @brief Current byte-count flow-control threshold (live-readable).
  [[nodiscard]] virtual std::size_t flow_control_bytes_threshold() const noexcept = 0;

  /// @brief Whether auto-spill on Add is enabled (maps from SpillPolicy today).
  [[nodiscard]] virtual bool auto_spill() const noexcept = 0;
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
    return bytes_added_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::size_t rows_added() const noexcept override {
    return rows_added_.load(std::memory_order_relaxed);
  }

  void SetFlowControl(std::size_t max_batches, std::size_t max_bytes) override;
  void SetSpillPolicy(bool enable_auto_spill) override;

  [[nodiscard]] std::size_t flow_control_batches_threshold() const noexcept override {
    return flow_control_batches_threshold_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::size_t flow_control_bytes_threshold() const noexcept override {
    return flow_control_bytes_threshold_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool auto_spill() const noexcept override {
    return auto_spill_.load(std::memory_order_acquire);
  }

 protected:
  /// @brief Notify threads waiting on saturation condition
  void NotifySaturationWaiters();

  /// @brief Track bytes and rows added
  void TrackAdded(std::size_t bytes, std::size_t rows);

  /// @brief Whether Add() should attempt spilling (reads live auto_spill_).
  [[nodiscard]] bool ShouldAutoSpill() const noexcept {
    return auto_spill_.load(std::memory_order_acquire);
  }

  /// @brief Live batch-count threshold used by WaitIfSaturated.
  [[nodiscard]] std::size_t FlowControlBatches() const noexcept {
    return flow_control_batches_threshold_.load(std::memory_order_acquire);
  }

  /// @brief Live byte-count threshold used by WaitIfSaturated.
  [[nodiscard]] std::size_t FlowControlBytes() const noexcept {
    return flow_control_bytes_threshold_.load(std::memory_order_acquire);
  }

  std::uint64_t id_;
  BatchHolderConfig config_;  ///< Construction-time values (timeout); FC/spill live below.
  BlockingQueue<DataBlockPtr> queue_;
  std::shared_ptr<memory::MemoryResourceManager> memory_manager_;

  std::atomic<std::size_t> bytes_added_{0};
  std::atomic<std::size_t> rows_added_{0};

  /// Live soft knobs — updated by SetFlowControl / SetSpillPolicy (Phase 2.5 D5).
  std::atomic<std::size_t> flow_control_batches_threshold_{
      std::numeric_limits<std::size_t>::max()};
  std::atomic<std::size_t> flow_control_bytes_threshold_{
      std::numeric_limits<std::size_t>::max()};
  std::atomic<bool> auto_spill_{true};

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
  [[nodiscard]] bool IsFinished() const override;
  void WaitIfSaturated() override;

 private:
  /// @brief Concatenate all pending batches into one
  [[nodiscard]] Result<DataBlockPtr> ConcatenateAll();

  /// @brief Chunked mode (concat_bytes_threshold > 0): drain the queue into
  /// pending_ once, then serve merged chunks of up to the byte cap per Pull.
  /// Always includes at least one block per chunk (an oversized single block
  /// still flows — the BlazingSQL/Sirius oversized-batch rule).
  [[nodiscard]] Result<DataBlockPtr> NextChunk();

  mutable std::mutex pending_mutex_;
  std::deque<DataBlockPtr> pending_;  ///< Drained blocks awaiting chunked delivery.
  bool drained_{false};               ///< Queue fully drained into pending_.
};

// =============================================================================
// NonWaitingBatchHolder Implementation
// =============================================================================

/// @brief Non-blocking holder for pre-filled, single-shot handoffs.
///
/// BlazingSQL heritage: `NonWaitingCacheMachine` — "non-blocking pop, assumes
/// data is already there" (the direct-execution path pre-loads a source cache,
/// then runs the kernel). Semantics in QUEST:
/// - `Pull()` never blocks: returns the next block, or Invalid when empty.
/// - **Empty means drained**: `IsFinished()` is true whenever the queue is
///   empty (no finished-latch required). Consumers therefore see clean EOS
///   the moment the pre-filled data runs out.
/// - Do NOT wire this on an edge whose producer runs concurrently in the same
///   graph: the consumer may observe empty before the producer publishes.
///   `GraphBuilder::Build()` rejects explicit FlowControl/Full on these edges.
class NonWaitingBatchHolder final : public BatchHolderBase {
 public:
  NonWaitingBatchHolder(
      std::uint64_t id, BatchHolderConfig config = {},
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

  ~NonWaitingBatchHolder() override = default;

  [[nodiscard]] BatchHolderType type() const noexcept override {
    return BatchHolderType::kNonWaiting;
  }

  [[nodiscard]] Status Add(DataBlockPtr data) override;
  [[nodiscard]] Result<DataBlockPtr> Pull() override;      // non-blocking
  [[nodiscard]] Result<DataBlockPtr> TryPull() override;   // same as Pull
  [[nodiscard]] bool HasNext() const override;
  [[nodiscard]] bool IsFinished() const override;          // empty == drained
  void WaitIfSaturated() override {}                       // never throttles
};

// =============================================================================
// Factory Function
// =============================================================================

/// @brief Create a batch holder of the specified type
[[nodiscard]] std::shared_ptr<IBatchHolder> MakeBatchHolder(
    std::uint64_t id, BatchHolderType type, BatchHolderConfig config = {},
    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

}  // namespace quest::cache

#endif  // QUEST_CACHE_BATCH_HOLDER_HPP_
