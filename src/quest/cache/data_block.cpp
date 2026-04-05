#include "quest/cache/data_block.hpp"

#include "quest/datasource/record_batch_serializer.hpp"

namespace quest::cache {

// =============================================================================
// HostMemoryBlock Implementation
// =============================================================================

HostMemoryBlock::HostMemoryBlock(
    std::string id, datasource::RecordBatch record_batch,
    std::shared_ptr<memory::CPUMemoryResource> memory_resource)
    : id_(std::move(id)),
      record_batch_(
          std::make_unique<datasource::RecordBatch>(std::move(record_batch))),
      cpu_resource_(std::move(memory_resource)) {
  if (cpu_resource_) {
    cpu_resource_->record_allocation(size_in_bytes());
  }
}

HostMemoryBlock::~HostMemoryBlock() { Release(); }

HostMemoryBlock::HostMemoryBlock(HostMemoryBlock&& other) noexcept
    : id_(std::move(other.id_)),
      record_batch_(std::move(other.record_batch_)),
      valid_(other.valid_),
      cpu_resource_(std::move(other.cpu_resource_)),
      disk_resource_(std::move(other.disk_resource_)) {
  other.valid_ = false;
}

HostMemoryBlock& HostMemoryBlock::operator=(HostMemoryBlock&& other) noexcept {
  if (this != &other) {
    Release();

    id_ = std::move(other.id_);
    record_batch_ = std::move(other.record_batch_);
    valid_ = other.valid_;
    cpu_resource_ = std::move(other.cpu_resource_);
    disk_resource_ = std::move(other.disk_resource_);

    other.valid_ = false;
  }
  return *this;
}

std::size_t HostMemoryBlock::size_in_bytes() const noexcept {
  if (!record_batch_) {
    return 0;
  }
  // Estimate size based on serialized format
  // This is an approximation; actual serialized size may vary
  std::size_t size = 0;
  for (std::size_t i = 0; i < record_batch_->num_columns(); ++i) {
    const auto& column = record_batch_->column(i);
    size += column.size() * 8;  // Rough estimate per value
  }
  return size;
}

Result<std::shared_ptr<HostMemoryBlock>> HostMemoryBlock::ToCpu() {
  // Already in CPU memory, return shared_ptr to this
  // Note: This requires the caller to already have a shared_ptr
  return Status::Invalid(
      "Cannot convert HostMemoryBlock to CPU - use existing shared_ptr");
}

Result<std::shared_ptr<DiskBlock>> HostMemoryBlock::ToDisk() {
  if (!valid_ || !record_batch_) {
    return Status::Invalid("Cache data is not valid");
  }

  if (!disk_resource_) {
    return Status::Invalid("No disk resource configured for spilling");
  }

  // Generate cache file path
  auto file_path = disk_resource_->generate_cache_path("cpu_spill");

  // Serialize and write record batch to disk
  try {
    datasource::RecordBatchSerializer::SerializeToFile(*record_batch_,
                                                        file_path.string());
  } catch (const std::exception& e) {
    return Status::IOError("Failed to serialize record batch: " +
                           std::string(e.what()));
  }

  // Get serialized size
  std::error_code ec;
  auto file_size = std::filesystem::file_size(file_path, ec);
  if (ec) {
    std::filesystem::remove(file_path);
    return Status::IOError("Failed to get file size: " + ec.message());
  }

  // Record file creation
  disk_resource_->record_file_created(file_path, file_size);

  // Create disk cache data
  auto disk_data = std::make_shared<DiskBlock>(
      id_, file_path, file_size, record_batch_->num_rows(),
      record_batch_->num_columns(), disk_resource_);

  // Set CPU resource for potential reload
  disk_data->set_cpu_resource(cpu_resource_);

  return disk_data;
}

void HostMemoryBlock::Release() {
  if (valid_ && cpu_resource_ && record_batch_) {
    cpu_resource_->record_deallocation(size_in_bytes());
  }
  valid_ = false;
  record_batch_.reset();
}

// =============================================================================
// DiskBlock Implementation
// =============================================================================

DiskBlock::DiskBlock(
    std::string id, std::filesystem::path file_path, std::size_t size_bytes,
    std::size_t num_rows, std::size_t num_columns,
    std::shared_ptr<memory::DiskMemoryResource> memory_resource)
    : id_(std::move(id)),
      file_path_(std::move(file_path)),
      size_bytes_(size_bytes),
      num_rows_(num_rows),
      num_columns_(num_columns),
      disk_resource_(std::move(memory_resource)) {
  // File already exists, no need to record creation here
}

DiskBlock::~DiskBlock() { Release(); }

DiskBlock::DiskBlock(DiskBlock&& other) noexcept
    : id_(std::move(other.id_)),
      file_path_(std::move(other.file_path_)),
      size_bytes_(other.size_bytes_),
      num_rows_(other.num_rows_),
      num_columns_(other.num_columns_),
      owns_file_(other.owns_file_),
      disk_resource_(std::move(other.disk_resource_)),
      cpu_resource_(std::move(other.cpu_resource_)) {
  other.owns_file_ = false;
  other.size_bytes_ = 0;
  other.num_rows_ = 0;
  other.num_columns_ = 0;
}

DiskBlock& DiskBlock::operator=(DiskBlock&& other) noexcept {
  if (this != &other) {
    Release();

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

bool DiskBlock::is_valid() const noexcept {
  return owns_file_ && std::filesystem::exists(file_path_);
}

Result<std::shared_ptr<HostMemoryBlock>> DiskBlock::ToCpu() {
  if (!is_valid()) {
    return Status::Invalid("Cache file is not valid");
  }

  // Deserialize record batch from file
  try {
    auto record_batch =
        datasource::RecordBatchSerializer::DeserializeFromFile(file_path_.string());

    // Create CPU cache data
    auto cpu_data = std::make_shared<HostMemoryBlock>(
        id_, std::move(record_batch), cpu_resource_);

    // Set disk resource for potential re-spill
    cpu_data->set_disk_resource(disk_resource_);

    return cpu_data;
  } catch (const std::exception& e) {
    return Status::IOError("Failed to deserialize record batch: " +
                           std::string(e.what()));
  }
}

Result<std::shared_ptr<DiskBlock>> DiskBlock::ToDisk() {
  // Already on disk
  return Status::Invalid(
      "Cannot convert DiskBlock to Disk - use existing shared_ptr");
}

void DiskBlock::Release() {
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
