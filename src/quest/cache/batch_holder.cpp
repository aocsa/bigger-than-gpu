#include "quest/cache/batch_holder.hpp"

#include <format>

namespace quest::cache {

// =============================================================================
// BatchHolderBase Implementation
// =============================================================================

BatchHolderBase::BatchHolderBase(
    std::uint64_t id,
    BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : id_(id)
    , config_(config)
    , queue_(QueueConfig{.timeout = config_.timeout})
    , memory_manager_(std::move(memory_manager)) {}

Status BatchHolderBase::add_with_message(std::string message_id, std::shared_ptr<CacheData> data) {
    auto message = std::make_unique<CacheMessage>(std::move(message_id), std::move(data));
    return add(std::move(message));
}

void BatchHolderBase::finish() {
    queue_.Finish();
    notify_saturation_waiters();
}

bool BatchHolderBase::is_finished() const {
    return queue_.IsFinished() && queue_.Empty();
}

std::size_t BatchHolderBase::batch_count() const {
    return queue_.Size();
}

void BatchHolderBase::notify_saturation_waiters() {
    {
        std::lock_guard lock(saturation_mutex_);
    }
    saturation_cv_.notify_all();
}

void BatchHolderBase::track_added(std::size_t bytes, std::size_t rows) {
    bytes_added_.fetch_add(bytes);
    rows_added_.fetch_add(rows);
}

// =============================================================================
// BatchHolder Implementation
// =============================================================================

BatchHolder::BatchHolder(
    std::uint64_t id,
    BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : BatchHolderBase(id, config, std::move(memory_manager)) {}

Status BatchHolder::add(CacheMessagePtr message) {
    if (!message) {
        return Status::Invalid("Cannot add null message");
    }

    auto bytes = message->size_in_bytes();
    auto rows = message->num_rows();

    // Check for spilling if auto_spill is enabled
    if (config_.auto_spill && memory_manager_) {
        auto status = maybe_spill(message);
        if (!status.ok()) {
            return status;
        }
    }

    auto status = queue_.Put(std::move(message));
    if (status.ok()) {
        track_added(bytes, rows);
        current_bytes_.fetch_add(bytes);
    }

    return status;
}

Status BatchHolder::add(std::string message_id, std::shared_ptr<CacheData> data) {
    return add_with_message(std::move(message_id), std::move(data));
}

Result<CacheMessagePtr> BatchHolder::pull() {
    auto result = queue_.Pop();
    if (result) {
        current_bytes_.fetch_sub((*result)->size_in_bytes());
        notify_saturation_waiters();
    }
    return result;
}

Result<CacheMessagePtr> BatchHolder::try_pull() {
    auto result = queue_.TryPop();
    if (result) {
        current_bytes_.fetch_sub((*result)->size_in_bytes());
        notify_saturation_waiters();
    }
    return result;
}

bool BatchHolder::has_next() const {
    return queue_.HasNext();
}

void BatchHolder::wait_if_saturated() {
    std::unique_lock lock(saturation_mutex_);

    saturation_cv_.wait(lock, [this] {
        if (queue_.IsFinished()) {
            return true;  // Don't block if finished
        }

        // Check batch count threshold
        if (queue_.BatchesCount() >= config_.flow_control_batches_threshold) {
            return false;
        }

        // Check bytes threshold
        if (current_bytes_.load() >= config_.flow_control_bytes_threshold) {
            return false;
        }

        return true;
    });
}

bool BatchHolder::should_spill() const {
    return memory_manager_ && memory_manager_->should_spill();
}

Status BatchHolder::maybe_spill(CacheMessagePtr& message) {
    if (!message || !message->data()) {
        return Status::OK();
    }

    auto& data = message->data();

    // Only spill CPU data if memory is saturated
    if (data->tier() == memory::MemoryTier::CPU && should_spill()) {
        auto disk_result = data->to_disk();
        if (disk_result) {
            // Replace with disk data
            message = std::make_unique<CacheMessage>(
                std::string(message->id()),
                std::move(*disk_result));
        }
        // If spilling fails, continue with CPU data
    }

    return Status::OK();
}

// =============================================================================
// ConcatenatingBatchHolder Implementation
// =============================================================================

ConcatenatingBatchHolder::ConcatenatingBatchHolder(
    std::uint64_t id,
    BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : BatchHolderBase(id, config, std::move(memory_manager)) {}

Status ConcatenatingBatchHolder::add(CacheMessagePtr message) {
    if (!message) {
        return Status::Invalid("Cannot add null message");
    }

    auto bytes = message->size_in_bytes();
    auto rows = message->num_rows();

    auto status = queue_.Put(std::move(message));
    if (status.ok()) {
        track_added(bytes, rows);
    }

    return status;
}

Status ConcatenatingBatchHolder::add(std::string message_id, std::shared_ptr<CacheData> data) {
    return add_with_message(std::move(message_id), std::move(data));
}

Result<CacheMessagePtr> ConcatenatingBatchHolder::pull() {
    // Wait until queue is finished (all data added) or has items
    auto status = queue_.WaitUntilFinished();
    if (!status.ok() && !queue_.HasNext()) {
        return std::unexpected(status.error());
    }

    return concatenate_all();
}

Result<CacheMessagePtr> ConcatenatingBatchHolder::try_pull() {
    if (!queue_.IsFinished()) {
        return Status::Invalid("Queue not finished yet");
    }

    return concatenate_all();
}

bool ConcatenatingBatchHolder::has_next() const {
    return queue_.HasNext() || (queue_.IsFinished() && queue_.Size() > 0);
}

void ConcatenatingBatchHolder::wait_if_saturated() {
    std::unique_lock lock(saturation_mutex_);

    saturation_cv_.wait(lock, [this] {
        if (queue_.IsFinished()) {
            return true;
        }
        return queue_.BatchesCount() < config_.flow_control_batches_threshold;
    });
}

Result<CacheMessagePtr> ConcatenatingBatchHolder::concatenate_all() {
    auto all_messages = queue_.PopAll();
    if (!all_messages) {
        return std::unexpected(all_messages.error());
    }

    auto& messages = *all_messages;
    if (messages.empty()) {
        return Status::Invalid("No messages to concatenate");
    }

    if (messages.size() == 1) {
        return std::move(messages[0]);
    }

    // Concatenate all data into a single buffer
    // Calculate total size
    std::size_t total_bytes = 0;
    std::size_t total_rows = 0;
    std::size_t num_columns = 0;

    for (const auto& msg : messages) {
        if (msg && msg->data()) {
            total_bytes += msg->data()->size_in_bytes();
            total_rows += msg->data()->num_rows();
            if (num_columns == 0) {
                num_columns = msg->data()->num_columns();
            }
        }
    }

    // Create concatenated buffer
    std::vector<std::byte> concatenated_data;
    concatenated_data.reserve(total_bytes);

    for (auto& msg : messages) {
        if (msg && msg->data()) {
            // Load from disk if needed
            auto& data = msg->data();
            if (data->tier() == memory::MemoryTier::Disk) {
                auto cpu_result = data->to_cpu();
                if (cpu_result) {
                    data = std::move(*cpu_result);
                }
            }

            // Append data
            if (auto* cpu_data = dynamic_cast<CPUCacheData*>(data.get())) {
                auto span = cpu_data->data();
                concatenated_data.insert(concatenated_data.end(), span.begin(), span.end());
            }
        }
    }

    // Create result
    auto cpu_resource = memory_manager_
        ? std::make_shared<memory::CPUMemoryResource>()
        : nullptr;

    auto result_data = std::make_shared<CPUCacheData>(
        std::format("concat_{}", id_),
        std::move(concatenated_data),
        total_rows,
        num_columns,
        cpu_resource);

    return std::make_unique<CacheMessage>(
        std::format("concat_{}", id_),
        std::move(result_data));
}

// =============================================================================
// Factory Function
// =============================================================================

std::shared_ptr<IBatchHolder> make_batch_holder(
    std::uint64_t id,
    BatchHolderType type,
    BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager) {

    switch (type) {
        case BatchHolderType::Simple:
            return std::make_shared<BatchHolder>(
                id, config, std::move(memory_manager));

        case BatchHolderType::Concatenating:
            return std::make_shared<ConcatenatingBatchHolder>(
                id, config, std::move(memory_manager));
    }

    // Default to simple
    return std::make_shared<BatchHolder>(
        id, config, std::move(memory_manager));
}

}  // namespace quest::cache
