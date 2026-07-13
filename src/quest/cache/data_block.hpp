#ifndef QUEST_CACHE_DATA_BLOCK_HPP_
#define QUEST_CACHE_DATA_BLOCK_HPP_

/// @file data_block.hpp
/// @brief Cache data representations for CPU and Disk tiers
/// @details DataBlock is the abstract representation of data that can be stored
///          in different memory tiers. Implementations handle the actual
///          storage.

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

#include "quest/datasource/record_batch.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"

namespace quest::cache {

// =============================================================================
// Cache Data Interface
// =============================================================================


class HostMemoryBlock;
class DiskBlock;

/// @brief Abstract base class for cached data in any memory tier
class DataBlock {
 public:
  virtual ~DataBlock() = default;

  /// @brief Get unique identifier for this cache entry
  [[nodiscard]] virtual std::string_view id() const noexcept = 0;

  /// @brief Get the memory tier where this data is stored
  [[nodiscard]] virtual memory::MemoryTier tier() const noexcept = 0;

  /// @brief Get the size in bytes of the cached data
  [[nodiscard]] virtual std::size_t size_in_bytes() const noexcept = 0;

  /// @brief Get the number of rows (if applicable)
  [[nodiscard]] virtual std::size_t num_rows() const noexcept = 0;

  /// @brief Get the number of columns (if applicable)
  [[nodiscard]] virtual std::size_t num_columns() const noexcept = 0;

  /// @brief Check if the data is still valid (not evicted/corrupted)
  [[nodiscard]] virtual bool is_valid() const noexcept = 0;

  /// @brief Load data into CPU memory (may be a no-op if already in CPU)
  [[nodiscard]] virtual Result<std::shared_ptr<HostMemoryBlock>> ToCpu() = 0;

  /// @brief Spill data to disk (may be a no-op if already on disk)
  [[nodiscard]] virtual Result<std::shared_ptr<DiskBlock>> ToDisk() = 0;

  /// @brief Release resources associated with this cache entry
  virtual void Release() = 0;

 protected:
  DataBlock() = default;

  // Non-copyable (data should be explicitly copied/moved)
  DataBlock(const DataBlock&) = delete;
  DataBlock& operator=(const DataBlock&) = delete;

  // Movable
  DataBlock(DataBlock&&) = default;
  DataBlock& operator=(DataBlock&&) = default;
};

// =============================================================================
// CPU Cache Data
// =============================================================================

/// @brief Cache data stored in CPU memory
/// @details Holds a RecordBatch in RAM
class HostMemoryBlock final : public DataBlock {
 public:
  /// @brief Construct from a RecordBatch
  explicit HostMemoryBlock(
      std::string id, datasource::RecordBatch record_batch,
      std::shared_ptr<memory::CPUMemoryResource> memory_resource = nullptr);

  ~HostMemoryBlock() override;

  // Move operations
  HostMemoryBlock(HostMemoryBlock&&) noexcept;
  HostMemoryBlock& operator=(HostMemoryBlock&&) noexcept;

  // DataBlock interface
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

  /// @brief Get read-only access to the record batch
  [[nodiscard]] const datasource::RecordBatch* record_batch() const noexcept {
    return record_batch_.get();
  }

  [[nodiscard]] datasource::RecordBatch* record_batch() noexcept {
    return record_batch_.get();
  }

  /// @brief Get mutable access to the record batch
  [[nodiscard]] datasource::RecordBatch* mutable_record_batch() noexcept {
    return record_batch_.get();
  }

  /// @brief Set the disk memory resource for spilling operations
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

/// @brief Cache data stored on disk
/// @details Holds a reference to a file on disk containing the cached data
class DiskBlock final : public DataBlock {
 public:
  /// @brief Construct from existing file
  DiskBlock(
      std::string id, std::filesystem::path file_path, std::size_t size_bytes,
      std::size_t num_rows, std::size_t num_columns,
      std::shared_ptr<memory::DiskMemoryResource> memory_resource = nullptr);

  ~DiskBlock() override;

  // Move operations
  DiskBlock(DiskBlock&&) noexcept;
  DiskBlock& operator=(DiskBlock&&) noexcept;

  // DataBlock interface
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

  /// @brief Get the file path
  [[nodiscard]] const std::filesystem::path& file_path() const noexcept {
    return file_path_;
  }

  /// @brief Set the CPU memory resource for loading operations
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

/// @brief Shared pointer type for DataBlock (for use with BlockingQueue)
using DataBlockPtr = std::shared_ptr<DataBlock>;

}  // namespace quest::cache

#endif  // QUEST_CACHE_DATA_BLOCK_HPP_
