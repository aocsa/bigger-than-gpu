#pragma once

/// @file cache_data.hpp
/// @brief Cache data representations for CPU and Disk tiers
/// @details CacheData is the abstract representation of data that can be stored
///          in different memory tiers. Implementations handle the actual storage.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "quest/memory/memory_resource.hpp"
#include "quest/result.hpp"

namespace quest::cache {

// Forward declarations
class Table;  // Abstract table representation (to be defined)

// =============================================================================
// Cache Data Interface
// =============================================================================

/// @brief Abstract base class for cached data in any memory tier
class CacheData {
public:
    virtual ~CacheData() = default;
    
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
    [[nodiscard]] virtual Result<std::shared_ptr<CacheData>> to_cpu() = 0;
    
    /// @brief Spill data to disk (may be a no-op if already on disk)
    [[nodiscard]] virtual Result<std::shared_ptr<CacheData>> to_disk() = 0;
    
    /// @brief Release resources associated with this cache entry
    virtual void release() = 0;

protected:
    CacheData() = default;
    
    // Non-copyable (data should be explicitly copied/moved)
    CacheData(const CacheData&) = delete;
    CacheData& operator=(const CacheData&) = delete;
    
    // Movable
    CacheData(CacheData&&) = default;
    CacheData& operator=(CacheData&&) = default;
};

// =============================================================================
// CPU Cache Data
// =============================================================================

/// @brief Cache data stored in CPU memory
/// @details Holds data as a contiguous byte buffer in RAM
class CPUCacheData final : public CacheData {
public:
    /// @brief Construct from existing data buffer
    CPUCacheData(
        std::string id,
        std::vector<std::byte> data,
        std::size_t num_rows,
        std::size_t num_columns,
        std::shared_ptr<memory::CPUMemoryResource> memory_resource = nullptr);
    
    /// @brief Construct with pre-allocated buffer
    CPUCacheData(
        std::string id,
        std::size_t size_bytes,
        std::size_t num_rows,
        std::size_t num_columns,
        std::shared_ptr<memory::CPUMemoryResource> memory_resource = nullptr);
    
    ~CPUCacheData() override;
    
    // Move operations
    CPUCacheData(CPUCacheData&&) noexcept;
    CPUCacheData& operator=(CPUCacheData&&) noexcept;
    
    // CacheData interface
    [[nodiscard]] std::string_view id() const noexcept override { return id_; }
    [[nodiscard]] memory::MemoryTier tier() const noexcept override { 
        return memory::MemoryTier::CPU; 
    }
    [[nodiscard]] std::size_t size_in_bytes() const noexcept override { return data_.size(); }
    [[nodiscard]] std::size_t num_rows() const noexcept override { return num_rows_; }
    [[nodiscard]] std::size_t num_columns() const noexcept override { return num_columns_; }
    [[nodiscard]] bool is_valid() const noexcept override { return valid_; }
    [[nodiscard]] Result<std::shared_ptr<CacheData>> to_cpu() override;
    [[nodiscard]] Result<std::shared_ptr<CacheData>> to_disk() override;
    void release() override;
    
    /// @brief Get read-only access to the data buffer
    [[nodiscard]] std::span<const std::byte> data() const noexcept { return data_; }
    
    /// @brief Get mutable access to the data buffer
    [[nodiscard]] std::span<std::byte> mutable_data() noexcept { return data_; }
    
    /// @brief Set the disk memory resource for spilling operations
    void set_disk_resource(std::shared_ptr<memory::DiskMemoryResource> disk_resource) {
        disk_resource_ = std::move(disk_resource);
    }

private:
    std::string id_;
    std::vector<std::byte> data_;
    std::size_t num_rows_;
    std::size_t num_columns_;
    bool valid_{true};
    std::shared_ptr<memory::CPUMemoryResource> cpu_resource_;
    std::shared_ptr<memory::DiskMemoryResource> disk_resource_;
};

// =============================================================================
// Disk Cache Data
// =============================================================================

/// @brief Cache data stored on disk
/// @details Holds a reference to a file on disk containing the cached data
class DiskCacheData final : public CacheData {
public:
    /// @brief Construct from existing file
    DiskCacheData(
        std::string id,
        std::filesystem::path file_path,
        std::size_t size_bytes,
        std::size_t num_rows,
        std::size_t num_columns,
        std::shared_ptr<memory::DiskMemoryResource> memory_resource = nullptr);
    
    ~DiskCacheData() override;
    
    // Move operations
    DiskCacheData(DiskCacheData&&) noexcept;
    DiskCacheData& operator=(DiskCacheData&&) noexcept;
    
    // CacheData interface
    [[nodiscard]] std::string_view id() const noexcept override { return id_; }
    [[nodiscard]] memory::MemoryTier tier() const noexcept override { 
        return memory::MemoryTier::Disk; 
    }
    [[nodiscard]] std::size_t size_in_bytes() const noexcept override { return size_bytes_; }
    [[nodiscard]] std::size_t num_rows() const noexcept override { return num_rows_; }
    [[nodiscard]] std::size_t num_columns() const noexcept override { return num_columns_; }
    [[nodiscard]] bool is_valid() const noexcept override;
    [[nodiscard]] Result<std::shared_ptr<CacheData>> to_cpu() override;
    [[nodiscard]] Result<std::shared_ptr<CacheData>> to_disk() override;
    void release() override;
    
    /// @brief Get the file path
    [[nodiscard]] const std::filesystem::path& file_path() const noexcept { return file_path_; }
    
    /// @brief Set the CPU memory resource for loading operations
    void set_cpu_resource(std::shared_ptr<memory::CPUMemoryResource> cpu_resource) {
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
// Message Wrapper for Queue
// =============================================================================

/// @brief Wrapper for CacheData to be used in BlockingQueue
/// @details Adds the id() method required by the HasId concept
class CacheMessage {
public:
    CacheMessage(std::string message_id, std::shared_ptr<CacheData> data)
        : message_id_(std::move(message_id))
        , data_(std::move(data)) {}
    
    [[nodiscard]] std::string_view id() const noexcept { return message_id_; }
    
    [[nodiscard]] std::shared_ptr<CacheData>& data() noexcept { return data_; }
    [[nodiscard]] const std::shared_ptr<CacheData>& data() const noexcept { return data_; }
    
    [[nodiscard]] std::size_t size_in_bytes() const noexcept {
        return data_ ? data_->size_in_bytes() : 0;
    }
    
    [[nodiscard]] std::size_t num_rows() const noexcept {
        return data_ ? data_->num_rows() : 0;
    }

private:
    std::string message_id_;
    std::shared_ptr<CacheData> data_;
};

/// @brief Unique pointer type for CacheMessage (for use with BlockingQueue)
using CacheMessagePtr = std::unique_ptr<CacheMessage>;

}  // namespace quest::cache
