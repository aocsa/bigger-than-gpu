#include "quest/cache/batch_holder.hpp"

namespace quest::cache {

// =============================================================================
// BatchHolderBase Implementation
// =============================================================================

BatchHolderBase::BatchHolderBase(
    std::uint64_t id, BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : id_(id),
      config_(config),
      queue_(QueueConfig{.timeout = config_.timeout}),
      memory_manager_(std::move(memory_manager)) {}

void BatchHolderBase::Finish() {
  queue_.Finish();
  NotifySaturationWaiters();
}

bool BatchHolderBase::IsFinished() const {
  return queue_.IsFinished() && queue_.Empty();
}

std::size_t BatchHolderBase::batch_count() const { return queue_.Size(); }

void BatchHolderBase::NotifySaturationWaiters() {
  { std::lock_guard lock(saturation_mutex_); }
  saturation_cv_.notify_all();
}

void BatchHolderBase::TrackAdded(std::size_t bytes, std::size_t rows) {
  bytes_added_.fetch_add(bytes);
  rows_added_.fetch_add(rows);
}

// =============================================================================
// BatchHolder Implementation
// =============================================================================

BatchHolder::BatchHolder(
    std::uint64_t id, BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : BatchHolderBase(id, config, std::move(memory_manager)) {}

Status BatchHolder::Add(DataBlockPtr data) {
  if (!data) {
    return Status::Invalid("Cannot add null data block");
  }

  auto bytes = data->size_in_bytes();
  auto rows = data->num_rows();

  // Check for spilling if auto_spill is enabled
  if (config_.auto_spill && memory_manager_) {
    auto status = MaybeSpill(data);
    if (!status.ok()) {
      return status;
    }
  }

  auto status = queue_.Put(std::move(data));
  if (status.ok()) {
    TrackAdded(bytes, rows);
    current_bytes_.fetch_add(bytes);
  }

  return status;
}

Result<DataBlockPtr> BatchHolder::Pull() {
  auto result = queue_.Pop();
  if (result) {
    current_bytes_.fetch_sub((*result)->size_in_bytes());
    NotifySaturationWaiters();
  }
  return result;
}

Result<DataBlockPtr> BatchHolder::TryPull() {
  auto result = queue_.TryPop();
  if (result) {
    current_bytes_.fetch_sub((*result)->size_in_bytes());
    NotifySaturationWaiters();
  }
  return result;
}

bool BatchHolder::HasNext() const { return queue_.HasNext(); }

void BatchHolder::WaitIfSaturated() {
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

bool BatchHolder::ShouldSpill() const {
  return memory_manager_ && memory_manager_->should_spill();
}

Status BatchHolder::MaybeSpill(DataBlockPtr& data) {
  if (!data) {
    return Status::OK();
  }

  // Only spill CPU data if memory is saturated
  if (data->tier() == memory::MemoryTier::CPU && ShouldSpill()) {
    auto disk_result = data->ToDisk();
    if (disk_result) {
      // Replace with disk data
      data = std::move(*disk_result);
    }
    // If spilling fails, continue with CPU data
  }

  return Status::OK();
}

// =============================================================================
// ConcatenatingBatchHolder Implementation
// =============================================================================

ConcatenatingBatchHolder::ConcatenatingBatchHolder(
    std::uint64_t id, BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : BatchHolderBase(id, config, std::move(memory_manager)) {}

Status ConcatenatingBatchHolder::Add(DataBlockPtr data) {
  if (!data) {
    return Status::Invalid("Cannot add null data block");
  }

  auto bytes = data->size_in_bytes();
  auto rows = data->num_rows();

  auto status = queue_.Put(std::move(data));
  if (status.ok()) {
    TrackAdded(bytes, rows);
  }

  return status;
}

Result<DataBlockPtr> ConcatenatingBatchHolder::Pull() {
  // Wait until queue is finished (all data added) or has items
  auto status = queue_.WaitUntilFinished();
  if (!status.ok() && !queue_.HasNext()) {
    return std::unexpected(status.error());
  }

  return ConcatenateAll();
}

Result<DataBlockPtr> ConcatenatingBatchHolder::TryPull() {
  if (!queue_.IsFinished()) {
    return Status::Invalid("Queue not finished yet");
  }

  return ConcatenateAll();
}

bool ConcatenatingBatchHolder::HasNext() const {
  return queue_.HasNext() || (queue_.IsFinished() && queue_.Size() > 0);
}

void ConcatenatingBatchHolder::WaitIfSaturated() {
  std::unique_lock lock(saturation_mutex_);

  saturation_cv_.wait(lock, [this] {
    if (queue_.IsFinished()) {
      return true;
    }
    return queue_.BatchesCount() < config_.flow_control_batches_threshold;
  });
}

Result<DataBlockPtr> ConcatenatingBatchHolder::ConcatenateAll() {
  auto all_blocks = queue_.PopAll();
  if (!all_blocks) {
    return std::unexpected(all_blocks.error());
  }

  auto& blocks = *all_blocks;
  if (blocks.empty()) {
    return Status::Invalid("No data blocks to concatenate");
  }

  if (blocks.size() == 1) {
    return std::move(blocks[0]);
  }

  // For multiple blocks, we need to concatenate RecordBatches
  // This requires combining column data from all batches
  // For now, return an error - proper implementation would require
  // creating new column vectors with combined data
  return Status::Invalid(
      "Concatenation of multiple RecordBatch blocks not yet implemented");
}

// =============================================================================
// Factory Function
// =============================================================================

std::shared_ptr<IBatchHolder> MakeBatchHolder(
    std::uint64_t id, BatchHolderType type, BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager) {
  switch (type) {
    case BatchHolderType::kSimple:
      return std::make_shared<BatchHolder>(id, config,
                                           std::move(memory_manager));

    case BatchHolderType::kConcatenating:
      return std::make_shared<ConcatenatingBatchHolder>(
          id, config, std::move(memory_manager));
  }

  // Default to simple
  return std::make_shared<BatchHolder>(id, config, std::move(memory_manager));
}

}  // namespace quest::cache
