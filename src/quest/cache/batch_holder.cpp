#include "quest/cache/batch_holder.hpp"

#include "quest/execution/batch_kernels.hpp"

namespace quest::cache {

// =============================================================================
// BatchHolderBase Implementation
// =============================================================================

BatchHolderBase::BatchHolderBase(std::uint64_t id, BatchHolderConfig config,
                                 std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : id_(id),
      config_(config),
      queue_(QueueConfig{.timeout = config_.timeout}),
      memory_manager_(std::move(memory_manager)),
      flow_control_batches_threshold_(config.flow_control_batches_threshold),
      flow_control_bytes_threshold_(config.flow_control_bytes_threshold),
      auto_spill_(config.auto_spill) {}

void BatchHolderBase::Finish() {
  queue_.Finish();
  NotifySaturationWaiters();
}

bool BatchHolderBase::IsFinished() const {
  return queue_.IsFinished() && queue_.Empty();
}

std::size_t BatchHolderBase::batch_count() const {
  return queue_.Size();
}

void BatchHolderBase::NotifySaturationWaiters() {
  {
    std::lock_guard lock(saturation_mutex_);
  }
  saturation_cv_.notify_all();
  // Phase 3: after Notify, also post a task-re-evaluation event to the
  // task creator/scheduler so WAITING producers are re-armed without relying
  // on an unrelated completion (today nobody blocks once phase 3 lands).
}

void BatchHolderBase::SetFlowControl(std::size_t max_batches, std::size_t max_bytes) {
  flow_control_batches_threshold_.store(max_batches, std::memory_order_release);
  flow_control_bytes_threshold_.store(max_bytes, std::memory_order_release);
  NotifySaturationWaiters();
}

void BatchHolderBase::SetSpillPolicy(bool enable_auto_spill) {
  auto_spill_.store(enable_auto_spill, std::memory_order_release);
}

void BatchHolderBase::TrackAdded(std::size_t bytes, std::size_t rows) {
  bytes_added_.fetch_add(bytes, std::memory_order_relaxed);
  rows_added_.fetch_add(rows, std::memory_order_relaxed);
}

// =============================================================================
// BatchHolder Implementation
// =============================================================================

BatchHolder::BatchHolder(std::uint64_t id, BatchHolderConfig config,
                         std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : BatchHolderBase(id, config, std::move(memory_manager)) {}

Status BatchHolder::Add(DataBlockPtr data) {
  if (!data) {
    return Status::Invalid("Cannot add null data block");
  }

  auto bytes = data->size_in_bytes();
  auto rows = data->num_rows();

  // Check for spilling if auto_spill is enabled
  if (ShouldAutoSpill() && memory_manager_) {
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

bool BatchHolder::HasNext() const {
  return queue_.HasNext();
}

void BatchHolder::WaitIfSaturated() {
  std::unique_lock lock(saturation_mutex_);

  saturation_cv_.wait(lock, [this] {
    if (queue_.IsFinished()) {
      return true;  // Don't block if finished
    }

    // Check batch count threshold (live atomic)
    if (queue_.BatchesCount() >= FlowControlBatches()) {
      return false;
    }

    // Check bytes threshold (live atomic)
    if (current_bytes_.load(std::memory_order_acquire) >= FlowControlBytes()) {
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
    return queue_.BatchesCount() < FlowControlBatches();
  });
}

Result<DataBlockPtr> ConcatenatingBatchHolder::ConcatenateAll() {
  auto all_blocks = queue_.PopAll();
  if (!all_blocks) {
    return std::unexpected(all_blocks.error());
  }

  auto& blocks = *all_blocks;
  return execution::ConcatenateHostBlocks(std::move(blocks));
}

// =============================================================================
// Factory Function
// =============================================================================

std::shared_ptr<IBatchHolder> MakeBatchHolder(
    std::uint64_t id, BatchHolderType type, BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager) {
  switch (type) {
    case BatchHolderType::kSimple:
      return std::make_shared<BatchHolder>(id, config, std::move(memory_manager));

    case BatchHolderType::kConcatenating:
      return std::make_shared<ConcatenatingBatchHolder>(id, config, std::move(memory_manager));
  }

  // Default to simple
  return std::make_shared<BatchHolder>(id, config, std::move(memory_manager));
}

}  // namespace quest::cache
