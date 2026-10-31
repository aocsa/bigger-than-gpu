#include "quest/memory/memory_resource.hpp"

#include <algorithm>
#include <format>
#include <fstream>
#include <stdexcept>

#include <unistd.h>

#ifdef __linux__
#include <sys/statvfs.h>
#endif

namespace quest::memory {

// =============================================================================
// Helper Functions
// =============================================================================

namespace {

/// @brief Get available system memory (Linux implementation)
[[nodiscard]] std::size_t get_available_system_memory() {
#ifdef __linux__
    std::size_t pages = sysconf(_SC_PHYS_PAGES);
    std::size_t page_size = sysconf(_SC_PAGE_SIZE);
    return pages * page_size;
#else
    // Default to 8GB for non-Linux systems
    return 8ULL * 1024 * 1024 * 1024;
#endif
}

/// @brief Get available disk space for a path
[[nodiscard]] std::size_t get_available_disk_space([[maybe_unused]] const std::filesystem::path& path) {
#ifdef __linux__
    struct statvfs stat;
    if (statvfs(path.c_str(), &stat) == 0) {
        return stat.f_bavail * stat.f_frsize;
    }
#endif
    // Default to 100GB for non-Linux or on error
    return 100ULL * 1024 * 1024 * 1024;
}

}  // namespace

// =============================================================================
// CPUMemoryResource Implementation
// =============================================================================

CPUMemoryResource::CPUMemoryResource(CPUMemoryConfig config)
    : config_(std::move(config)) {
    if (config_.max_bytes == 0) {
        config_.max_bytes = get_available_system_memory();
    }
}

MemoryStats CPUMemoryResource::stats() const {
    std::lock_guard lock(mutex_);
    return MemoryStats{
        .total_bytes = config_.max_bytes,
        .used_bytes = used_bytes_.load(),
        .peak_bytes = peak_bytes_.load(),
        .allocation_count = allocation_count_.load()
    };
}

bool CPUMemoryResource::can_allocate(std::size_t bytes) const {
    return used_bytes_.load() + bytes <= config_.max_bytes;
}

bool CPUMemoryResource::is_saturated() const {
    double utilization = static_cast<double>(used_bytes_.load()) / 
                         static_cast<double>(config_.max_bytes);
    return utilization >= config_.saturation_threshold;
}

double CPUMemoryResource::saturation_threshold() const noexcept {
    return config_.saturation_threshold;
}

void CPUMemoryResource::set_saturation_threshold(double threshold) {
    std::lock_guard lock(mutex_);
    config_.saturation_threshold = std::clamp(threshold, 0.0, 1.0);
}

void CPUMemoryResource::record_allocation(std::size_t bytes) {
    std::size_t current = used_bytes_.fetch_add(bytes) + bytes;
    
    std::size_t peak = peak_bytes_.load();
    while (current > peak && !peak_bytes_.compare_exchange_weak(peak, current)) {
    }
    
    if (config_.track_allocations) {
        allocation_count_.fetch_add(1);
    }
}

void CPUMemoryResource::record_deallocation(std::size_t bytes) {
    used_bytes_.fetch_sub(bytes);
    if (config_.track_allocations) {
        allocation_count_.fetch_sub(1);
    }
}

// =============================================================================
// DiskMemoryResource Implementation
// =============================================================================

namespace {
// Several resources (queries, processes) may share one base_path; each spills into and cleans
// up only its own subdirectory.
std::filesystem::path unique_spill_dir(const std::filesystem::path& base_path) {
    static std::atomic<std::uint64_t> next_instance{0};
    return base_path / std::format("quest_{}_{}", ::getpid(), next_instance.fetch_add(1));
}
}  // namespace

DiskMemoryResource::DiskMemoryResource(DiskMemoryConfig config)
    : config_(std::move(config)), spill_dir_(unique_spill_dir(config_.base_path)) {
    std::filesystem::create_directories(spill_dir_);
    
    if (config_.max_bytes == 0) {
        config_.max_bytes = get_available_disk_space(config_.base_path);
    }
}

DiskMemoryResource::~DiskMemoryResource() {
    if (config_.cleanup_on_destroy) {
        std::error_code ec;
        std::filesystem::remove_all(spill_dir_, ec);
    }
}

MemoryStats DiskMemoryResource::stats() const {
    std::lock_guard lock(mutex_);
    return MemoryStats{
        .total_bytes = config_.max_bytes,
        .used_bytes = used_bytes_.load(),
        .peak_bytes = peak_bytes_.load(),
        .allocation_count = file_count_.load()
    };
}

bool DiskMemoryResource::can_allocate(std::size_t bytes) const {
    return used_bytes_.load() + bytes <= config_.max_bytes;
}

bool DiskMemoryResource::is_saturated() const {
    double utilization = static_cast<double>(used_bytes_.load()) / 
                         static_cast<double>(config_.max_bytes);
    return utilization >= config_.saturation_threshold;
}

double DiskMemoryResource::saturation_threshold() const noexcept {
    return config_.saturation_threshold;
}

void DiskMemoryResource::set_saturation_threshold(double threshold) {
    std::lock_guard lock(mutex_);
    config_.saturation_threshold = std::clamp(threshold, 0.0, 1.0);
}

std::filesystem::path DiskMemoryResource::generate_cache_path(std::string_view prefix) {
    std::uint64_t counter = file_counter_.fetch_add(1);
    auto filename = std::format("{}_{}.cache", prefix, counter);
    return spill_dir_ / filename;
}

void DiskMemoryResource::record_file_created(
        const std::filesystem::path& /*path*/, 
        std::size_t bytes) {
    std::size_t current = used_bytes_.fetch_add(bytes) + bytes;
    
    std::size_t peak = peak_bytes_.load();
    while (current > peak && !peak_bytes_.compare_exchange_weak(peak, current)) {
    }
    
    file_count_.fetch_add(1);
}

void DiskMemoryResource::record_file_deleted(
        const std::filesystem::path& /*path*/, 
        std::size_t bytes) {
    used_bytes_.fetch_sub(bytes);
    file_count_.fetch_sub(1);
}

// =============================================================================
// MemoryResourceManager Implementation
// =============================================================================

MemoryResourceManager::MemoryResourceManager(
    std::shared_ptr<CPUMemoryResource> cpu_resource,
    std::shared_ptr<DiskMemoryResource> disk_resource)
    : cpu_resource_(std::move(cpu_resource))
    , disk_resource_(std::move(disk_resource)) {
    if (!cpu_resource_) {
        throw std::invalid_argument("CPU memory resource cannot be null");
    }
    if (!disk_resource_) {
        throw std::invalid_argument("Disk memory resource cannot be null");
    }
}

MemoryResource& MemoryResourceManager::get(MemoryTier tier) {
    switch (tier) {
        case MemoryTier::CPU:  return *cpu_resource_;
        case MemoryTier::Disk: return *disk_resource_;
    }
    throw std::invalid_argument("Unknown memory tier");
}

const MemoryResource& MemoryResourceManager::get(MemoryTier tier) const {
    switch (tier) {
        case MemoryTier::CPU:  return *cpu_resource_;
        case MemoryTier::Disk: return *disk_resource_;
    }
    throw std::invalid_argument("Unknown memory tier");
}

MemoryTier MemoryResourceManager::recommend_tier(std::size_t bytes) const {
    if (cpu_resource_->can_allocate(bytes) && !cpu_resource_->is_saturated()) {
        return MemoryTier::CPU;
    }
    return MemoryTier::Disk;
}

bool MemoryResourceManager::should_spill() const {
    return cpu_resource_->is_saturated();
}

}  // namespace quest::memory
