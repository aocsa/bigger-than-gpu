#pragma once

/// @file data_block.hpp
/// @brief Abstract cache data representation across CPU and Disk tiers.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "quest/columnar/record_batch.hpp"
#include "quest/common/result.hpp"
#include "quest/memory/memory_resource.hpp"

namespace quest::cache {

// =============================================================================
// Cache Data Interface
// =============================================================================


class HostMemoryBlock;
class DiskBlock;

class DataBlock {
 public:
  virtual ~DataBlock() = default;

  [[nodiscard]] virtual std::string_view id() const noexcept = 0;
  [[nodiscard]] virtual memory::MemoryTier tier() const noexcept = 0;
  [[nodiscard]] virtual std::size_t size_in_bytes() const noexcept = 0;
  [[nodiscard]] virtual std::size_t num_rows() const noexcept = 0;
  [[nodiscard]] virtual std::size_t num_columns() const noexcept = 0;

  /// Returns false if the data has been evicted or corrupted.
  [[nodiscard]] virtual bool is_valid() const noexcept = 0;

  /// No-op if already in CPU memory.
  [[nodiscard]] virtual Result<std::shared_ptr<HostMemoryBlock>> ToCpu() = 0;

  /// No-op if already on disk.
  [[nodiscard]] virtual Result<std::shared_ptr<DiskBlock>> ToDisk() = 0;

  virtual void Release() = 0;

 protected:
  DataBlock() = default;

  // Non-copyable (data should be explicitly copied/moved)
  DataBlock(const DataBlock&) = delete;
  DataBlock& operator=(const DataBlock&) = delete;

  DataBlock(DataBlock&&) = default;
  DataBlock& operator=(DataBlock&&) = default;
};

// =============================================================================
// CPU Cache Data
// =============================================================================

/// Holds a RecordBatch in CPU (RAM).
class HostMemoryBlock final : public DataBlock {
 public:
  explicit HostMemoryBlock(
      std::string id, datasource::RecordBatch record_batch,
      std::shared_ptr<memory::CPUMemoryResource> memory_resource = nullptr);

  ~HostMemoryBlock() override;

  HostMemoryBlock(HostMemoryBlock&&) noexcept;
  HostMemoryBlock& operator=(HostMemoryBlock&&) noexcept;

  [[nodiscard]] std::string_view id() const noexcept override { return id_; }
  [[nodiscard]] memory::MemoryTier tier() const noexcept override {
    return memory::MemoryTier::CPU;
  }
  [[nodiscard]] std::size_t size_in_bytes() const noexcept override;
  [[nodiscard]] std::size_t num_rows() const noexcept override {
    return record_batch_ ? record_batch_->num_rows() : 0;
  }
  [[nodiscard]] std::size_t num_columns() const noexcept override {
    return record_batch_ ? record_batch_->num_columns() : 0;
  }
  [[nodiscard]] bool is_valid() const noexcept override { return valid_; }
  [[nodiscard]] Result<std::shared_ptr<HostMemoryBlock>> ToCpu() override;
  [[nodiscard]] Result<std::shared_ptr<DiskBlock>> ToDisk() override;
  void Release() override;

  [[nodiscard]] const datasource::RecordBatch* record_batch() const noexcept {
    return record_batch_.get();
  }

  [[nodiscard]] datasource::RecordBatch* record_batch() noexcept {
    return record_batch_.get();
  }

  [[nodiscard]] datasource::RecordBatch* mutable_record_batch() noexcept {
    return record_batch_.get();
  }

  void set_disk_resource(
      std::shared_ptr<memory::DiskMemoryResource> disk_resource) {
    disk_resource_ = std::move(disk_resource);
  }

 private:
  std::string id_;
  std::unique_ptr<datasource::RecordBatch> record_batch_;
  bool valid_{true};
  std::shared_ptr<memory::CPUMemoryResource> cpu_resource_;
  std::shared_ptr<memory::DiskMemoryResource> disk_resource_;
};

// =============================================================================
// Disk Cache Data
// =============================================================================

/// Holds a reference to a spill file on disk.
class DiskBlock final : public DataBlock {
 public:
  DiskBlock(
      std::string id, std::filesystem::path file_path, std::size_t size_bytes,
      std::size_t num_rows, std::size_t num_columns,
      std::shared_ptr<memory::DiskMemoryResource> memory_resource = nullptr);

  ~DiskBlock() override;

  DiskBlock(DiskBlock&&) noexcept;
  DiskBlock& operator=(DiskBlock&&) noexcept;

  [[nodiscard]] std::string_view id() const noexcept override { return id_; }
  [[nodiscard]] memory::MemoryTier tier() const noexcept override {
    return memory::MemoryTier::Disk;
  }
  [[nodiscard]] std::size_t size_in_bytes() const noexcept override {
    return size_bytes_;
  }
  [[nodiscard]] std::size_t num_rows() const noexcept override {
    return num_rows_;
  }
  [[nodiscard]] std::size_t num_columns() const noexcept override {
    return num_columns_;
  }
  [[nodiscard]] bool is_valid() const noexcept override;
  [[nodiscard]] Result<std::shared_ptr<HostMemoryBlock>> ToCpu() override;
  [[nodiscard]] Result<std::shared_ptr<DiskBlock>> ToDisk() override;
  void Release() override;

  [[nodiscard]] const std::filesystem::path& file_path() const noexcept {
    return file_path_;
  }

  void set_cpu_resource(
      std::shared_ptr<memory::CPUMemoryResource> cpu_resource) {
    cpu_resource_ = std::move(cpu_resource);
  }

 private:
  std::string id_;
  std::filesystem::path file_path_;
  std::size_t size_bytes_;
  std::size_t num_rows_;
  std::size_t num_columns_;
  bool owns_file_{true};  // Whether to delete file on destruction
  std::shared_ptr<memory::DiskMemoryResource> disk_resource_;
  std::shared_ptr<memory::CPUMemoryResource> cpu_resource_;
};

// =============================================================================
// Type Alias for BlockingQueue
// =============================================================================

using DataBlockPtr = std::shared_ptr<DataBlock>;

// =============================================================================
// Host Block Helpers
// =============================================================================

/// @brief Ensure a DataBlock is resident in CPU and return its HostMemoryBlock.
[[nodiscard]] Result<std::shared_ptr<HostMemoryBlock>> EnsureHostBlock(DataBlockPtr input);

/// @brief Wrap a RecordBatch in a HostMemoryBlock carrying both memory
/// resources from @p memory_manager (which may be null).
///
/// Every operator that creates output blocks must use this instead of a
/// bare HostMemoryBlock: without the CPU resource the allocation is
/// invisible to saturation accounting, and without the disk resource
/// ToDisk() fails — MaybeSpill then silently keeps the data in RAM, so a
/// plan with any filter/project before a pipeline break could never spill.
[[nodiscard]] std::shared_ptr<HostMemoryBlock> MakeTrackedHostBlock(
    std::string block_id, datasource::RecordBatch batch,
    const std::shared_ptr<memory::MemoryResourceManager>& memory_manager);

/// @brief Column-wise concatenate of HostMemoryBlock RecordBatches.
[[nodiscard]] Result<DataBlockPtr> ConcatenateHostBlocks(std::vector<DataBlockPtr> blocks);

}  // namespace quest::cache
