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
    return Error::Invalid("Cannot add null data block");
  }

  auto bytes = data->size_in_bytes();
  auto rows = data->num_rows();

  if (ShouldAutoSpill() && memory_manager_) {
    auto status = MaybeSpill(data);
    if (!status.has_value()) {
      return status;
    }
  }

  auto status = queue_.Put(std::move(data));
  if (status.has_value()) {
    TrackAdded(bytes, rows);
    current_bytes_.fetch_add(bytes);
  }

  return status;
}

Result<std::optional<DataBlockPtr>> BatchHolder::Pull() {
  auto result = queue_.Pop();
  if (result && *result) {
    current_bytes_.fetch_sub((**result)->size_in_bytes());
    NotifySaturationWaiters();
  }
  return result;
}

Result<std::optional<DataBlockPtr>> BatchHolder::TryPull() {
  auto result = queue_.TryPop();
  if (result && *result) {
    current_bytes_.fetch_sub((**result)->size_in_bytes());
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
      return true;
    }

    if (queue_.BatchesCount() >= FlowControlBatches()) {
      return false;
    }

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
    return {};
  }

  // Only spill CPU data if memory is saturated
  if (data->tier() == memory::MemoryTier::CPU && ShouldSpill()) {
    auto disk_result = data->ToDisk();
    if (disk_result) {
      data = std::move(*disk_result);
    }
    // If spilling fails, continue with CPU data
  }

  return {};
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
    return Error::Invalid("Cannot add null data block");
  }

  auto bytes = data->size_in_bytes();
  auto rows = data->num_rows();

  auto status = queue_.Put(std::move(data));
  if (status.has_value()) {
    TrackAdded(bytes, rows);
  }

  return status;
}

Result<std::optional<DataBlockPtr>> ConcatenatingBatchHolder::Pull() {
  auto status = queue_.WaitUntilFinished();
  if (!status.has_value() && !queue_.HasNext()) {
    return std::unexpected(status.error());
  }

  // Chunked mode: serve merged chunks of up to concat_bytes_threshold bytes.
  if (config_.concat_bytes_threshold > 0) {
    return NextChunk();
  }

  return ConcatenateAll();
}

Result<std::optional<DataBlockPtr>> ConcatenatingBatchHolder::TryPull() {
  if (!queue_.IsFinished()) {
    return std::nullopt;
  }

  if (config_.concat_bytes_threshold > 0) {
    return NextChunk();
  }

  return ConcatenateAll();
}

bool ConcatenatingBatchHolder::HasNext() const {
  {
    std::lock_guard lock(pending_mutex_);
    if (!pending_.empty()) {
      return true;
    }
  }
  return queue_.HasNext() || (queue_.IsFinished() && queue_.Size() > 0);
}

bool ConcatenatingBatchHolder::IsFinished() const {
  {
    std::lock_guard lock(pending_mutex_);
    if (!pending_.empty()) {
      return false;
    }
  }
  return queue_.IsFinished() && queue_.Empty();
}

Result<std::optional<DataBlockPtr>> ConcatenatingBatchHolder::NextChunk() {
  std::lock_guard lock(pending_mutex_);

  // Drain the (finished) queue into pending_ exactly once.
  if (!drained_) {
    auto all_blocks = queue_.PopAll();
    if (all_blocks) {
      for (auto& block : *all_blocks) {
        pending_.push_back(std::move(block));
      }
    }
    drained_ = true;
  }

  if (pending_.empty()) {
    return std::nullopt;
  }

  // Greedy: take blocks while under the cap — but always at least one, so a
  // single oversized block still flows (the oversized-batch rule).
  const std::size_t cap = config_.concat_bytes_threshold;
  std::vector<DataBlockPtr> chunk;
  std::size_t chunk_bytes = 0;
  while (!pending_.empty()) {
    const std::size_t next_bytes = pending_.front()->size_in_bytes();
    if (!chunk.empty() && chunk_bytes + next_bytes > cap) {
      break;
    }
    chunk_bytes += next_bytes;
    chunk.push_back(std::move(pending_.front()));
    pending_.pop_front();
  }

  if (chunk.size() == 1) {
    return std::move(chunk.front());
  }
  return execution::ConcatenateHostBlocks(std::move(chunk));
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

Result<std::optional<DataBlockPtr>> ConcatenatingBatchHolder::ConcatenateAll() {
  auto all_blocks = queue_.PopAll();
  if (!all_blocks) {
    return std::unexpected(all_blocks.error());
  }

  auto& blocks = *all_blocks;
  if (blocks.empty()) {
    return std::nullopt;
  }
  return execution::ConcatenateHostBlocks(std::move(blocks));
}

// =============================================================================
// NonWaitingBatchHolder Implementation
// =============================================================================

NonWaitingBatchHolder::NonWaitingBatchHolder(
    std::uint64_t id, BatchHolderConfig config,
    std::shared_ptr<memory::MemoryResourceManager> memory_manager)
    : BatchHolderBase(id, config, std::move(memory_manager)) {}

Status NonWaitingBatchHolder::Add(DataBlockPtr data) {
  if (!data) {
    return Error::Invalid("Cannot add null data block");
  }

  auto bytes = data->size_in_bytes();
  auto rows = data->num_rows();

  auto status = queue_.Put(std::move(data));
  if (status.has_value()) {
    TrackAdded(bytes, rows);
  }
  return status;
}

Result<std::optional<DataBlockPtr>> NonWaitingBatchHolder::Pull() {
  // Non-blocking by contract: the data is pre-filled or it isn't coming.
  return queue_.TryPop();
}

Result<std::optional<DataBlockPtr>> NonWaitingBatchHolder::TryPull() { return Pull(); }

bool NonWaitingBatchHolder::HasNext() const { return queue_.HasNext(); }

bool NonWaitingBatchHolder::IsFinished() const {
  // Empty == drained: no finished-latch needed for single-shot handoffs.
  return queue_.Empty();
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

    case BatchHolderType::kNonWaiting:
      return std::make_shared<NonWaitingBatchHolder>(id, config, std::move(memory_manager));
  }

  return std::make_shared<BatchHolder>(id, config, std::move(memory_manager));
}

}  // namespace quest::cache
