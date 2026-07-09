#ifndef QUEST_MEMORY_MEMORY_RESOURCE_HPP_
#define QUEST_MEMORY_MEMORY_RESOURCE_HPP_

/// @file memory_resource.hpp
/// @brief Memory resource interfaces for CPU and Disk memory tiers
/// @details Implements a two-tier memory hierarchy:
///          - Tier 1 (L1): CPU RAM - primary storage for active data
///          - Tier 2 (L2): Disk - secondary storage when CPU RAM is exhausted

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

#include "quest/result.hpp"

namespace quest::memory {

// =============================================================================
// Memory Tier Enumeration
// =============================================================================

/// @brief Memory tier levels in the hierarchy
enum class MemoryTier : std::uint8_t {
  CPU = 0,  ///< Primary tier: CPU RAM
  Disk = 1  ///< Secondary tier: Disk storage
};

/// @brief Convert memory tier to string representation
[[nodiscard]] constexpr std::string_view to_string(MemoryTier tier) noexcept {
  switch (tier) {
    case MemoryTier::CPU:
      return "CPU";
    case MemoryTier::Disk:
      return "Disk";
  }
  return "Unknown";
}

// =============================================================================
// Memory Statistics
// =============================================================================

/// @brief Statistics for memory resource usage
struct MemoryStats {
  std::size_t total_bytes{0};       ///< Total bytes available
  std::size_t used_bytes{0};        ///< Currently used bytes
  std::size_t peak_bytes{0};        ///< Peak usage
  std::size_t allocation_count{0};  ///< Number of active allocations

  [[nodiscard]] std::size_t available_bytes() const noexcept {
    return total_bytes > used_bytes ? total_bytes - used_bytes : 0;
  }

  [[nodiscard]] double utilization() const noexcept {
    return total_bytes > 0 ? static_cast<double>(used_bytes) / static_cast<double>(total_bytes)
                           : 0.0;
  }
};

// =============================================================================
// Base Memory Resource Interface
// =============================================================================

/// @brief Abstract base class for memory resources
class MemoryResource {
 public:
  virtual ~MemoryResource() = default;

  /// @brief Get the memory tier this resource represents
  [[nodiscard]] virtual MemoryTier tier() const noexcept = 0;

  /// @brief Get current memory statistics
  [[nodiscard]] virtual MemoryStats stats() const = 0;

  /// @brief Check if there's enough space for an allocation
  [[nodiscard]] virtual bool can_allocate(std::size_t bytes) const = 0;

  /// @brief Get the resource name for logging/debugging
  [[nodiscard]] virtual std::string_view name() const noexcept = 0;

  /// @brief Check if this tier is saturated (needs spilling)
  [[nodiscard]] virtual bool is_saturated() const = 0;

  /// @brief Get saturation threshold (0.0 to 1.0)
  [[nodiscard]] virtual double saturation_threshold() const noexcept = 0;

  /// @brief Set saturation threshold
  virtual void set_saturation_threshold(double threshold) = 0;
};

// =============================================================================
// CPU Memory Resource
// =============================================================================

/// @brief Configuration for CPU memory resource
struct CPUMemoryConfig {
  std::size_t max_bytes{0};           ///< 0 = use system available
  double saturation_threshold{0.85};  ///< Threshold for spilling to disk
  bool track_allocations{true};       ///< Whether to track allocation count
};

/// @brief Memory resource for CPU RAM tier
class CPUMemoryResource final : public MemoryResource {
 public:
  explicit CPUMemoryResource(CPUMemoryConfig config = {});
  ~CPUMemoryResource() override = default;

  // Non-copyable, non-movable
  CPUMemoryResource(const CPUMemoryResource&) = delete;
  CPUMemoryResource& operator=(const CPUMemoryResource&) = delete;
  CPUMemoryResource(CPUMemoryResource&&) = delete;
  CPUMemoryResource& operator=(CPUMemoryResource&&) = delete;

  // MemoryResource interface
  [[nodiscard]] MemoryTier tier() const noexcept override { return MemoryTier::CPU; }
  [[nodiscard]] MemoryStats stats() const override;
  [[nodiscard]] bool can_allocate(std::size_t bytes) const override;
  [[nodiscard]] std::string_view name() const noexcept override { return "CPUMemoryResource"; }
  [[nodiscard]] bool is_saturated() const override;
  [[nodiscard]] double saturation_threshold() const noexcept override;
  void set_saturation_threshold(double threshold) override;

  /// @brief Record an allocation (for tracking purposes)
  void record_allocation(std::size_t bytes);

  /// @brief Record a deallocation (for tracking purposes)
  void record_deallocation(std::size_t bytes);

 private:
  CPUMemoryConfig config_;
  mutable std::mutex mutex_;
  std::atomic<std::size_t> used_bytes_{0};
  std::atomic<std::size_t> peak_bytes_{0};
  std::atomic<std::size_t> allocation_count_{0};
};

// =============================================================================
// Disk Memory Resource
// =============================================================================

/// @brief Configuration for disk memory resource
struct DiskMemoryConfig {
  std::filesystem::path base_path{"/tmp/quest_cache"};  ///< Base directory for cache files
  std::size_t max_bytes{0};                             ///< 0 = use available disk space
  double saturation_threshold{0.90};                    ///< Threshold for disk full warning
  bool cleanup_on_destroy{true};                        ///< Remove cache files on destruction
};

/// @brief Memory resource for Disk storage tier
class DiskMemoryResource final : public MemoryResource {
 public:
  explicit DiskMemoryResource(DiskMemoryConfig config = {});
  ~DiskMemoryResource() override;

  // Non-copyable, non-movable
  DiskMemoryResource(const DiskMemoryResource&) = delete;
  DiskMemoryResource& operator=(const DiskMemoryResource&) = delete;
  DiskMemoryResource(DiskMemoryResource&&) = delete;
  DiskMemoryResource& operator=(DiskMemoryResource&&) = delete;

  // MemoryResource interface
  [[nodiscard]] MemoryTier tier() const noexcept override { return MemoryTier::Disk; }
  [[nodiscard]] MemoryStats stats() const override;
  [[nodiscard]] bool can_allocate(std::size_t bytes) const override;
  [[nodiscard]] std::string_view name() const noexcept override { return "DiskMemoryResource"; }
  [[nodiscard]] bool is_saturated() const override;
  [[nodiscard]] double saturation_threshold() const noexcept override;
  void set_saturation_threshold(double threshold) override;

  /// @brief Get the base path for cache files
  [[nodiscard]] const std::filesystem::path& base_path() const noexcept {
    return config_.base_path;
  }

  /// @brief Generate a unique file path for a new cache file
  [[nodiscard]] std::filesystem::path generate_cache_path(std::string_view prefix = "cache");

  /// @brief Record file creation
  void record_file_created(const std::filesystem::path& path, std::size_t bytes);

  /// @brief Record file deletion
  void record_file_deleted(const std::filesystem::path& path, std::size_t bytes);

 private:
  DiskMemoryConfig config_;
  mutable std::mutex mutex_;
  std::atomic<std::size_t> used_bytes_{0};
  std::atomic<std::size_t> peak_bytes_{0};
  std::atomic<std::size_t> file_count_{0};
  std::atomic<std::uint64_t> file_counter_{0};  // For unique file names
};

// =============================================================================
// Memory Resource Manager
// =============================================================================

/// @brief Manages the two-tier memory hierarchy
class MemoryResourceManager {
 public:
  MemoryResourceManager(std::shared_ptr<CPUMemoryResource> cpu_resource,
                        std::shared_ptr<DiskMemoryResource> disk_resource);

  /// @brief Get the CPU memory resource
  [[nodiscard]] CPUMemoryResource& cpu() { return *cpu_resource_; }
  [[nodiscard]] const CPUMemoryResource& cpu() const { return *cpu_resource_; }
  [[nodiscard]] std::shared_ptr<CPUMemoryResource> cpu_ptr() const { return cpu_resource_; }

  /// @brief Get the Disk memory resource
  [[nodiscard]] DiskMemoryResource& disk() { return *disk_resource_; }
  [[nodiscard]] const DiskMemoryResource& disk() const { return *disk_resource_; }
  [[nodiscard]] std::shared_ptr<DiskMemoryResource> disk_ptr() const { return disk_resource_; }

  /// @brief Get resource by tier
  [[nodiscard]] MemoryResource& get(MemoryTier tier);
  [[nodiscard]] const MemoryResource& get(MemoryTier tier) const;

  /// @brief Determine the best tier for a new allocation
  [[nodiscard]] MemoryTier recommend_tier(std::size_t bytes) const;

  /// @brief Check if data should be spilled from CPU to disk
  [[nodiscard]] bool should_spill() const;

 private:
  std::shared_ptr<CPUMemoryResource> cpu_resource_;
  std::shared_ptr<DiskMemoryResource> disk_resource_;
};

}  // namespace quest::memory

#endif  // QUEST_MEMORY_MEMORY_RESOURCE_HPP_
