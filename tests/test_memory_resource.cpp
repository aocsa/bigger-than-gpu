// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_memory_resource.cpp
/// @brief Unit tests for MemoryResource implementations.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>

#include "quest/memory/memory_resource.hpp"
#include "quest/tests/utils.hpp"

using namespace quest::memory;

// =============================================================================
// MemoryTier Tests
// =============================================================================

TEST(MemoryTierTest, ToString) {
  EXPECT_EQ(to_string(MemoryTier::CPU), "CPU");
  EXPECT_EQ(to_string(MemoryTier::Disk), "Disk");
}

// =============================================================================
// MemoryStats Tests
// =============================================================================

TEST(MemoryStatsTest, AvailableBytes) {
  MemoryStats stats;
  stats.total_bytes = 1000;
  stats.used_bytes = 400;

  EXPECT_EQ(stats.available_bytes(), 600U);
}

TEST(MemoryStatsTest, AvailableBytesOverflow) {
  MemoryStats stats;
  stats.total_bytes = 100;
  stats.used_bytes = 200;  // More than total

  EXPECT_EQ(stats.available_bytes(), 0U);
}

TEST(MemoryStatsTest, Utilization) {
  MemoryStats stats;
  stats.total_bytes = 1000;
  stats.used_bytes = 500;

  EXPECT_NEAR(stats.utilization(), 0.5, 0.001);
}

TEST(MemoryStatsTest, UtilizationZeroTotal) {
  MemoryStats stats;
  stats.total_bytes = 0;
  stats.used_bytes = 0;

  EXPECT_NEAR(stats.utilization(), 0.0, 0.001);
}

TEST(MemoryStatsTest, UtilizationFull) {
  MemoryStats stats;
  stats.total_bytes = 1000;
  stats.used_bytes = 1000;

  EXPECT_NEAR(stats.utilization(), 1.0, 0.001);
}

// =============================================================================
// CPUMemoryResource Tests
// =============================================================================

TEST(CPUMemoryResourceTest, DefaultConstruction) {
  CPUMemoryResource resource;

  EXPECT_EQ(resource.tier(), MemoryTier::CPU);
  EXPECT_EQ(resource.name(), "CPUMemoryResource");

  auto stats = resource.stats();
  EXPECT_GT(stats.total_bytes, 0U);
  EXPECT_EQ(stats.used_bytes, 0U);
  EXPECT_EQ(stats.peak_bytes, 0U);
  EXPECT_EQ(stats.allocation_count, 0U);
}

TEST(CPUMemoryResourceTest, WithConfig) {
  CPUMemoryConfig config;
  config.max_bytes = 1024 * 1024;
  config.saturation_threshold = 0.75;

  CPUMemoryResource resource(config);

  auto stats = resource.stats();
  EXPECT_EQ(stats.total_bytes, 1024U * 1024U);
  EXPECT_NEAR(resource.saturation_threshold(), 0.75, 0.001);
}

TEST(CPUMemoryResourceTest, RecordAllocation) {
  CPUMemoryConfig config;
  config.max_bytes = 10000;
  CPUMemoryResource resource(config);

  resource.record_allocation(1000);
  resource.record_allocation(500);

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 1500U);
  EXPECT_EQ(stats.peak_bytes, 1500U);
  EXPECT_EQ(stats.allocation_count, 2U);
}

TEST(CPUMemoryResourceTest, RecordDeallocation) {
  CPUMemoryConfig config;
  config.max_bytes = 10000;
  CPUMemoryResource resource(config);

  resource.record_allocation(1000);
  resource.record_allocation(500);
  resource.record_deallocation(300);

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 1200U);
  EXPECT_EQ(stats.peak_bytes, 1500U);  // Peak unchanged
  EXPECT_EQ(stats.allocation_count, 1U);
}

TEST(CPUMemoryResourceTest, CanAllocate) {
  CPUMemoryConfig config;
  config.max_bytes = 1000;
  CPUMemoryResource resource(config);

  EXPECT_TRUE(resource.can_allocate(500));
  EXPECT_TRUE(resource.can_allocate(1000));
  EXPECT_FALSE(resource.can_allocate(1001));

  resource.record_allocation(600);
  EXPECT_TRUE(resource.can_allocate(400));
  EXPECT_FALSE(resource.can_allocate(401));
}

TEST(CPUMemoryResourceTest, IsSaturated) {
  CPUMemoryConfig config;
  config.max_bytes = 1000;
  config.saturation_threshold = 0.80;
  CPUMemoryResource resource(config);

  EXPECT_FALSE(resource.is_saturated());

  resource.record_allocation(700);
  EXPECT_FALSE(resource.is_saturated());

  resource.record_allocation(100);  // Now at 80%
  EXPECT_TRUE(resource.is_saturated());
}

TEST(CPUMemoryResourceTest, SetSaturationThreshold) {
  CPUMemoryConfig config;
  config.max_bytes = 1000;
  config.saturation_threshold = 0.80;
  CPUMemoryResource resource(config);

  resource.set_saturation_threshold(0.50);
  EXPECT_NEAR(resource.saturation_threshold(), 0.50, 0.001);

  // Test clamping to max
  resource.set_saturation_threshold(1.5);
  EXPECT_NEAR(resource.saturation_threshold(), 1.0, 0.001);

  // Test clamping to min
  resource.set_saturation_threshold(-0.5);
  EXPECT_NEAR(resource.saturation_threshold(), 0.0, 0.001);
}

TEST(CPUMemoryResourceTest, PeakTracking) {
  CPUMemoryConfig config;
  config.max_bytes = 10000;
  CPUMemoryResource resource(config);

  resource.record_allocation(500);
  resource.record_allocation(500);    // Peak = 1000
  resource.record_deallocation(300);  // Used = 700, peak = 1000
  resource.record_allocation(100);    // Used = 800, peak = 1000
  resource.record_allocation(300);    // Used = 1100, peak = 1100

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 1100U);
  EXPECT_EQ(stats.peak_bytes, 1100U);
}

// =============================================================================
// DiskMemoryResource Tests
// =============================================================================

class DiskMemoryResourceTest : public ::testing::Test {
 protected:
  void TearDown() override {
    // Clean up any test directories
    for (const auto& path : test_paths_) {
      std::filesystem::remove_all(path);
    }
  }

  DiskMemoryConfig MakeConfig(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    test_paths_.push_back(path);
    return DiskMemoryConfig{.base_path = path, .cleanup_on_destroy = true};
  }

  std::vector<std::filesystem::path> test_paths_;
};

TEST_F(DiskMemoryResourceTest, DefaultConstruction) {
  auto config = MakeConfig("test_disk_1");

  std::filesystem::path spill_dir;
  {
    DiskMemoryResource resource(config);

    EXPECT_EQ(resource.tier(), MemoryTier::Disk);
    EXPECT_EQ(resource.name(), "DiskMemoryResource");
    EXPECT_EQ(resource.base_path(), config.base_path);
    spill_dir = resource.generate_cache_path().parent_path();
    EXPECT_TRUE(std::filesystem::exists(spill_dir));

    auto stats = resource.stats();
    EXPECT_GT(stats.total_bytes, 0U);
    EXPECT_EQ(stats.used_bytes, 0U);
  }

  EXPECT_FALSE(std::filesystem::exists(spill_dir));
}

TEST_F(DiskMemoryResourceTest, WithMaxBytes) {
  auto config = MakeConfig("test_disk_2");
  config.max_bytes = 1024 * 1024 * 100;
  config.saturation_threshold = 0.85;

  DiskMemoryResource resource(config);

  auto stats = resource.stats();
  EXPECT_EQ(stats.total_bytes, 1024U * 1024U * 100U);
  EXPECT_NEAR(resource.saturation_threshold(), 0.85, 0.001);
}

TEST_F(DiskMemoryResourceTest, GenerateCachePath) {
  auto config = MakeConfig("test_disk_3");
  DiskMemoryResource resource(config);

  auto path1 = resource.generate_cache_path("test");
  auto path2 = resource.generate_cache_path("test");
  auto path3 = resource.generate_cache_path("other");

  // Paths should be unique
  EXPECT_NE(path1, path2);
  EXPECT_NE(path2, path3);

  // Paths should be in base directory
  EXPECT_EQ(path1.parent_path().parent_path(), config.base_path);
}

TEST_F(DiskMemoryResourceTest, RecordFileOperations) {
  auto config = MakeConfig("test_disk_4");
  config.max_bytes = 10000;
  DiskMemoryResource resource(config);

  resource.record_file_created("file1.cache", 1000);
  resource.record_file_created("file2.cache", 500);

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 1500U);
  EXPECT_EQ(stats.allocation_count, 2U);

  resource.record_file_deleted("file1.cache", 1000);

  stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 500U);
  EXPECT_EQ(stats.allocation_count, 1U);
}

TEST_F(DiskMemoryResourceTest, Saturation) {
  auto config = MakeConfig("test_disk_5");
  config.max_bytes = 1000;
  config.saturation_threshold = 0.90;
  DiskMemoryResource resource(config);

  EXPECT_FALSE(resource.is_saturated());

  resource.record_file_created("file1.cache", 800);
  EXPECT_FALSE(resource.is_saturated());

  resource.record_file_created("file2.cache", 100);  // Now at 90%
  EXPECT_TRUE(resource.is_saturated());
}

TEST_F(DiskMemoryResourceTest, CanAllocate) {
  auto config = MakeConfig("test_disk_6");
  config.max_bytes = 1000;
  DiskMemoryResource resource(config);

  EXPECT_TRUE(resource.can_allocate(1000));
  EXPECT_FALSE(resource.can_allocate(1001));

  resource.record_file_created("file1.cache", 600);
  EXPECT_TRUE(resource.can_allocate(400));
  EXPECT_FALSE(resource.can_allocate(401));
}

// =============================================================================
// MemoryResourceManager Tests
// =============================================================================

class MemoryResourceManagerTest : public ::testing::Test {
 protected:
  void TearDown() override {
    for (const auto& path : test_paths_) {
      std::filesystem::remove_all(path);
    }
  }

  std::shared_ptr<DiskMemoryResource> MakeDisk(const std::string& name) {
    auto path = std::filesystem::temp_directory_path() / name;
    test_paths_.push_back(path);
    DiskMemoryConfig config{.base_path = path, .cleanup_on_destroy = true};
    return std::make_shared<DiskMemoryResource>(config);
  }

  std::vector<std::filesystem::path> test_paths_;
};

TEST_F(MemoryResourceManagerTest, Construction) {
  auto cpu = std::make_shared<CPUMemoryResource>();
  auto disk = MakeDisk("test_manager_1");

  MemoryResourceManager manager(cpu, disk);

  EXPECT_EQ(&manager.cpu(), cpu.get());
  EXPECT_EQ(&manager.disk(), disk.get());
}

TEST_F(MemoryResourceManagerTest, NullResources) {
  auto cpu = std::make_shared<CPUMemoryResource>();
  auto disk = MakeDisk("test_manager_2");

  EXPECT_THROW(MemoryResourceManager(nullptr, disk), std::invalid_argument);
  EXPECT_THROW(MemoryResourceManager(cpu, nullptr), std::invalid_argument);
}

TEST_F(MemoryResourceManagerTest, GetByTier) {
  auto cpu = std::make_shared<CPUMemoryResource>();
  auto disk = MakeDisk("test_manager_3");

  MemoryResourceManager manager(cpu, disk);

  EXPECT_EQ(manager.get(MemoryTier::CPU).tier(), MemoryTier::CPU);
  EXPECT_EQ(manager.get(MemoryTier::Disk).tier(), MemoryTier::Disk);
}

TEST_F(MemoryResourceManagerTest, RecommendTier) {
  CPUMemoryConfig cpu_config;
  cpu_config.max_bytes = 1000;
  cpu_config.saturation_threshold = 0.80;
  auto cpu = std::make_shared<CPUMemoryResource>(cpu_config);

  auto path = std::filesystem::temp_directory_path() / "test_manager_4";
  test_paths_.push_back(path);
  DiskMemoryConfig disk_config{
      .base_path = path, .max_bytes = 10000, .cleanup_on_destroy = true};
  auto disk = std::make_shared<DiskMemoryResource>(disk_config);

  MemoryResourceManager manager(cpu, disk);

  // With empty CPU, should recommend CPU
  EXPECT_EQ(manager.recommend_tier(500), MemoryTier::CPU);

  // Saturate CPU
  cpu->record_allocation(800);

  // Now should recommend Disk
  EXPECT_EQ(manager.recommend_tier(100), MemoryTier::Disk);

  // Large allocation should go to Disk
  EXPECT_EQ(manager.recommend_tier(500), MemoryTier::Disk);
}

TEST_F(MemoryResourceManagerTest, ShouldSpill) {
  CPUMemoryConfig cpu_config;
  cpu_config.max_bytes = 1000;
  cpu_config.saturation_threshold = 0.80;
  auto cpu = std::make_shared<CPUMemoryResource>(cpu_config);
  auto disk = MakeDisk("test_manager_5");

  MemoryResourceManager manager(cpu, disk);

  EXPECT_FALSE(manager.should_spill());

  cpu->record_allocation(700);
  EXPECT_FALSE(manager.should_spill());

  cpu->record_allocation(100);  // Now at 80%
  EXPECT_TRUE(manager.should_spill());
}

// =============================================================================
// Edge Cases
// =============================================================================

TEST(MemoryResourceEdgeCaseTest, VerySmallMaxBytes) {
  CPUMemoryConfig config;
  config.max_bytes = 10;  // Very small allocation limit
  CPUMemoryResource resource(config);

  EXPECT_TRUE(resource.can_allocate(5));
  EXPECT_FALSE(resource.can_allocate(100));  // Too large
}

TEST(MemoryResourceEdgeCaseTest, ExactSaturation) {
  CPUMemoryConfig config;
  config.max_bytes = 100;
  config.saturation_threshold = 0.50;
  CPUMemoryResource resource(config);

  resource.record_allocation(49);
  EXPECT_FALSE(resource.is_saturated());

  resource.record_allocation(1);  // Exactly 50%
  EXPECT_TRUE(resource.is_saturated());
}

TEST(MemoryResourceEdgeCaseTest, CorrectDeallocation) {
  CPUMemoryConfig config;
  config.max_bytes = 1000;
  CPUMemoryResource resource(config);

  resource.record_allocation(100);
  resource.record_deallocation(100);  // Deallocate exactly what was allocated

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 0U);
}
