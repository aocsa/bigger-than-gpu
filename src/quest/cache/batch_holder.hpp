#pragma once

/// @file batch_holder.hpp
/// @brief BatchHolder - the cache machine for holding data batches between operators
/// @details Renamed from CacheMachine. Manages data flow between operators using a
///          blocking queue and handles spilling to disk when CPU memory is exhausted.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include "quest/cache/blocking_queue.hpp"
#include "quest/cache/cache_data.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"

namespace quest::cache {

// =============================================================================
// BatchHolder Configuration
// =============================================================================

/// @brief Configuration for BatchHolder
struct BatchHolderConfig {
    std::size_t flow_control_batches_threshold{std::numeric_limits<std::size_t>::max()};
    std::size_t flow_control_bytes_threshold{std::numeric_limits<std::size_t>::max()};
    std::chrono::milliseconds timeout{0};  // 0 = no timeout
    bool auto_spill{true};                  // Automatically spill to disk when CPU is saturated
};

/// @brief Type of batch holder (controls dequeue behavior)
enum class BatchHolderType : std::uint8_t {
    Simple,        ///< Each batch is pulled individually
    Concatenating  ///< All batches are concatenated into a single result
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
    
    /// @brief Add a cache data entry to the holder
    [[nodiscard]] virtual Status add(CacheMessagePtr message) = 0;
    
    /// @brief Add data directly (creates message internally)
    [[nodiscard]] virtual Status add(
        std::string message_id, 
        std::shared_ptr<CacheData> data) = 0;
    
    /// @brief Retrieve the next batch (blocks if empty)
    [[nodiscard]] virtual Result<CacheMessagePtr> pull() = 0;
    
    /// @brief Try to retrieve without blocking
    [[nodiscard]] virtual Result<CacheMessagePtr> try_pull() = 0;
    
    /// @brief Mark the holder as finished (no more data will be added)
    virtual void finish() = 0;
    
    /// @brief Check if the holder is finished AND empty
    [[nodiscard]] virtual bool is_finished() const = 0;
    
    /// @brief Check if there are batches available
    [[nodiscard]] virtual bool has_next() const = 0;
    
    /// @brief Get the number of batches currently in the holder
    [[nodiscard]] virtual std::size_t batch_count() const = 0;
    
    /// @brief Get the total bytes added to this holder
    [[nodiscard]] virtual std::size_t bytes_added() const noexcept = 0;
    
    /// @brief Get the total rows added to this holder
    [[nodiscard]] virtual std::size_t rows_added() const noexcept = 0;
    
    /// @brief Wait until the holder is not saturated (for flow control)
    virtual void wait_if_saturated() = 0;
};

// =============================================================================
// BatchHolderBase - Common implementation
// =============================================================================

/// @brief Base class providing common batch holder functionality
class BatchHolderBase : public IBatchHolder {
public:
    BatchHolderBase(
        std::uint64_t id,
        BatchHolderConfig config,
        std::shared_ptr<memory::MemoryResourceManager> memory_manager);

    ~BatchHolderBase() override = default;

    // Non-copyable, non-movable
    BatchHolderBase(const BatchHolderBase&) = delete;
    BatchHolderBase& operator=(const BatchHolderBase&) = delete;
    BatchHolderBase(BatchHolderBase&&) = delete;
    BatchHolderBase& operator=(BatchHolderBase&&) = delete;

    // Common IBatchHolder implementations
    [[nodiscard]] std::uint64_t id() const noexcept override { return id_; }
    void finish() override;
    [[nodiscard]] bool is_finished() const override;
    [[nodiscard]] std::size_t batch_count() const override;
    [[nodiscard]] std::size_t bytes_added() const noexcept override { return bytes_added_.load(); }
    [[nodiscard]] std::size_t rows_added() const noexcept override { return rows_added_.load(); }

protected:
    /// @brief Notify threads waiting on saturation condition
    void notify_saturation_waiters();

    /// @brief Track bytes and rows added
    void track_added(std::size_t bytes, std::size_t rows);

    /// @brief Helper to add data by creating a message (calls derived add())
    [[nodiscard]] Status add_with_message(std::string message_id, std::shared_ptr<CacheData> data);

    std::uint64_t id_;
    BatchHolderConfig config_;
    BlockingQueue<CacheMessagePtr> queue_;
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
        std::uint64_t id,
        BatchHolderConfig config = {},
        std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

    ~BatchHolder() override = default;

    // IBatchHolder interface
    [[nodiscard]] BatchHolderType type() const noexcept override {
        return BatchHolderType::Simple;
    }

    [[nodiscard]] Status add(CacheMessagePtr message) override;
    [[nodiscard]] Status add(std::string message_id, std::shared_ptr<CacheData> data) override;
    [[nodiscard]] Result<CacheMessagePtr> pull() override;
    [[nodiscard]] Result<CacheMessagePtr> try_pull() override;
    [[nodiscard]] bool has_next() const override;
    void wait_if_saturated() override;

private:
    /// @brief Check if we should spill data to disk
    [[nodiscard]] bool should_spill() const;

    /// @brief Spill a message to disk if needed
    [[nodiscard]] Status maybe_spill(CacheMessagePtr& message);

    std::atomic<std::size_t> current_bytes_{0};
};

// =============================================================================
// ConcatenatingBatchHolder Implementation
// =============================================================================

/// @brief Batch holder that concatenates all batches before returning
class ConcatenatingBatchHolder final : public BatchHolderBase {
public:
    ConcatenatingBatchHolder(
        std::uint64_t id,
        BatchHolderConfig config = {},
        std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

    ~ConcatenatingBatchHolder() override = default;

    // IBatchHolder interface
    [[nodiscard]] BatchHolderType type() const noexcept override {
        return BatchHolderType::Concatenating;
    }

    [[nodiscard]] Status add(CacheMessagePtr message) override;
    [[nodiscard]] Status add(std::string message_id, std::shared_ptr<CacheData> data) override;
    [[nodiscard]] Result<CacheMessagePtr> pull() override;
    [[nodiscard]] Result<CacheMessagePtr> try_pull() override;
    [[nodiscard]] bool has_next() const override;
    void wait_if_saturated() override;

private:
    /// @brief Concatenate all pending batches into one
    [[nodiscard]] Result<CacheMessagePtr> concatenate_all();
};

// =============================================================================
// Factory Function
// =============================================================================

/// @brief Create a batch holder of the specified type
[[nodiscard]] std::shared_ptr<IBatchHolder> make_batch_holder(
    std::uint64_t id,
    BatchHolderType type,
    BatchHolderConfig config = {},
    std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);

}  // namespace quest::cache
