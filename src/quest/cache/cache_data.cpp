#include "quest/cache/cache_data.hpp"

#include <cstring>
#include <fstream>

namespace quest::cache {

// =============================================================================
// CPUCacheData Implementation
// =============================================================================

CPUCacheData::CPUCacheData(
    std::string id,
    std::vector<std::byte> data,
    std::size_t num_rows,
    std::size_t num_columns,
    std::shared_ptr<memory::CPUMemoryResource> memory_resource)
    : id_(std::move(id))
    , data_(std::move(data))
    , num_rows_(num_rows)
    , num_columns_(num_columns)
    , cpu_resource_(std::move(memory_resource)) {
    
    if (cpu_resource_) {
        cpu_resource_->record_allocation(data_.size());
    }
}

CPUCacheData::CPUCacheData(
    std::string id,
    std::size_t size_bytes,
    std::size_t num_rows,
    std::size_t num_columns,
    std::shared_ptr<memory::CPUMemoryResource> memory_resource)
    : id_(std::move(id))
    , data_(size_bytes)
    , num_rows_(num_rows)
    , num_columns_(num_columns)
    , cpu_resource_(std::move(memory_resource)) {
    
    if (cpu_resource_) {
        cpu_resource_->record_allocation(data_.size());
    }
}

CPUCacheData::~CPUCacheData() {
    release();
}

CPUCacheData::CPUCacheData(CPUCacheData&& other) noexcept
    : id_(std::move(other.id_))
    , data_(std::move(other.data_))
    , num_rows_(other.num_rows_)
    , num_columns_(other.num_columns_)
    , valid_(other.valid_)
    , cpu_resource_(std::move(other.cpu_resource_))
    , disk_resource_(std::move(other.disk_resource_)) {
    other.valid_ = false;
    other.num_rows_ = 0;
    other.num_columns_ = 0;
}

CPUCacheData& CPUCacheData::operator=(CPUCacheData&& other) noexcept {
    if (this != &other) {
        release();
        
        id_ = std::move(other.id_);
        data_ = std::move(other.data_);
        num_rows_ = other.num_rows_;
        num_columns_ = other.num_columns_;
        valid_ = other.valid_;
        cpu_resource_ = std::move(other.cpu_resource_);
        disk_resource_ = std::move(other.disk_resource_);
        
        other.valid_ = false;
        other.num_rows_ = 0;
        other.num_columns_ = 0;
    }
    return *this;
}

Result<std::shared_ptr<CacheData>> CPUCacheData::to_cpu() {
    // Already in CPU memory, return shared_ptr to this
    // Note: This requires the caller to already have a shared_ptr
    return Status::Invalid("Cannot convert CPUCacheData to CPU - use existing shared_ptr");
}

Result<std::shared_ptr<CacheData>> CPUCacheData::to_disk() {
    if (!valid_) {
        return Status::Invalid("Cache data is not valid");
    }

    if (!disk_resource_) {
        return Status::Invalid("No disk resource configured for spilling");
    }

    // Generate cache file path
    auto file_path = disk_resource_->generate_cache_path("cpu_spill");

    // Write data to disk
    std::ofstream file(file_path, std::ios::binary);
    if (!file) {
        return Status::IOError("Failed to create cache file: " + file_path.string());
    }

    file.write(reinterpret_cast<const char*>(data_.data()),
               static_cast<std::streamsize>(data_.size()));

    if (!file) {
        std::filesystem::remove(file_path);
        return Status::IOError("Failed to write cache file: " + file_path.string());
    }
    
    file.close();
    
    // Record file creation
    disk_resource_->record_file_created(file_path, data_.size());
    
    // Create disk cache data
    auto disk_data = std::make_shared<DiskCacheData>(
        id_,
        file_path,
        data_.size(),
        num_rows_,
        num_columns_,
        disk_resource_);
    
    // Set CPU resource for potential reload
    disk_data->set_cpu_resource(cpu_resource_);
    
    return disk_data;
}

void CPUCacheData::release() {
    if (valid_ && cpu_resource_ && !data_.empty()) {
        cpu_resource_->record_deallocation(data_.size());
    }
    valid_ = false;
    data_.clear();
    data_.shrink_to_fit();
}

// =============================================================================
// DiskCacheData Implementation
// =============================================================================

DiskCacheData::DiskCacheData(
    std::string id,
    std::filesystem::path file_path,
    std::size_t size_bytes,
    std::size_t num_rows,
    std::size_t num_columns,
    std::shared_ptr<memory::DiskMemoryResource> memory_resource)
    : id_(std::move(id))
    , file_path_(std::move(file_path))
    , size_bytes_(size_bytes)
    , num_rows_(num_rows)
    , num_columns_(num_columns)
    , disk_resource_(std::move(memory_resource)) {
    // File already exists, no need to record creation here
}

DiskCacheData::~DiskCacheData() {
    release();
}

DiskCacheData::DiskCacheData(DiskCacheData&& other) noexcept
    : id_(std::move(other.id_))
    , file_path_(std::move(other.file_path_))
    , size_bytes_(other.size_bytes_)
    , num_rows_(other.num_rows_)
    , num_columns_(other.num_columns_)
    , owns_file_(other.owns_file_)
    , disk_resource_(std::move(other.disk_resource_))
    , cpu_resource_(std::move(other.cpu_resource_)) {
    other.owns_file_ = false;
    other.size_bytes_ = 0;
    other.num_rows_ = 0;
    other.num_columns_ = 0;
}

DiskCacheData& DiskCacheData::operator=(DiskCacheData&& other) noexcept {
    if (this != &other) {
        release();
        
        id_ = std::move(other.id_);
        file_path_ = std::move(other.file_path_);
        size_bytes_ = other.size_bytes_;
        num_rows_ = other.num_rows_;
        num_columns_ = other.num_columns_;
        owns_file_ = other.owns_file_;
        disk_resource_ = std::move(other.disk_resource_);
        cpu_resource_ = std::move(other.cpu_resource_);
        
        other.owns_file_ = false;
        other.size_bytes_ = 0;
        other.num_rows_ = 0;
        other.num_columns_ = 0;
    }
    return *this;
}

bool DiskCacheData::is_valid() const noexcept {
    return owns_file_ && std::filesystem::exists(file_path_);
}

Result<std::shared_ptr<CacheData>> DiskCacheData::to_cpu() {
    if (!is_valid()) {
        return Status::Invalid("Cache file is not valid");
    }

    // Read file into memory
    std::ifstream file(file_path_, std::ios::binary | std::ios::ate);
    if (!file) {
        return Status::IOError("Failed to open cache file: " + file_path_.string());
    }

    auto file_size = static_cast<std::size_t>(file.tellg());
    file.seekg(0, std::ios::beg);

    std::vector<std::byte> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()),
              static_cast<std::streamsize>(file_size));

    if (!file) {
        return Status::IOError("Failed to read cache file: " + file_path_.string());
    }
    
    // Create CPU cache data
    auto cpu_data = std::make_shared<CPUCacheData>(
        id_,
        std::move(data),
        num_rows_,
        num_columns_,
        cpu_resource_);
    
    // Set disk resource for potential re-spill
    cpu_data->set_disk_resource(disk_resource_);
    
    return cpu_data;
}

Result<std::shared_ptr<CacheData>> DiskCacheData::to_disk() {
    // Already on disk
    return Status::Invalid("Cannot convert DiskCacheData to Disk - use existing shared_ptr");
}

void DiskCacheData::release() {
    if (owns_file_ && !file_path_.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(file_path_, ec)) {
            if (disk_resource_) {
                disk_resource_->record_file_deleted(file_path_, size_bytes_);
            }
            std::filesystem::remove(file_path_, ec);
        }
    }
    owns_file_ = false;
    file_path_.clear();
    size_bytes_ = 0;
}

}  // namespace quest::cache
