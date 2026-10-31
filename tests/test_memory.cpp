// Copyright 2024
// Licensed under the Apache License, Version 2.0

/// @file test_memory.cpp
/// @brief Unit tests for memory module: MemoryResource, MemoryStats, MemoryResourceManager.

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include "quest/memory/memory_resource.hpp"

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

  EXPECT_EQ(stats.available_bytes(), 600);
}

TEST(MemoryStatsTest, AvailableBytesOverflow) {
  MemoryStats stats;
  stats.total_bytes = 100;
  stats.used_bytes = 200;  // More than total (shouldn't happen, but be safe)

  EXPECT_EQ(stats.available_bytes(), 0);
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

// =============================================================================
// CPUMemoryResource Tests
// =============================================================================

TEST(CPUMemoryResourceTest, DefaultConstruction) {
  CPUMemoryResource resource;

  EXPECT_EQ(resource.tier(), MemoryTier::CPU);
  EXPECT_EQ(resource.name(), "CPUMemoryResource");

  auto stats = resource.stats();
  EXPECT_GT(stats.total_bytes, 0U);  // Should have auto-detected system memory
  EXPECT_EQ(stats.used_bytes, 0U);
  EXPECT_EQ(stats.peak_bytes, 0U);
  EXPECT_EQ(stats.allocation_count, 0U);
}

TEST(CPUMemoryResourceTest, WithConfig) {
  CPUMemoryConfig config;
  config.max_bytes = 1024 * 1024;  // 1 MB
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
  EXPECT_EQ(stats.peak_bytes, 1500U);  // Peak should still be 1500
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

  // Test clamping
  resource.set_saturation_threshold(1.5);  // Should clamp to 1.0
  EXPECT_NEAR(resource.saturation_threshold(), 1.0, 0.001);

  resource.set_saturation_threshold(-0.5);  // Should clamp to 0.0
  EXPECT_NEAR(resource.saturation_threshold(), 0.0, 0.001);
}

TEST(CPUMemoryResourceTest, PeakTracking) {
  CPUMemoryConfig config;
  config.max_bytes = 10000;
  CPUMemoryResource resource(config);

  resource.record_allocation(500);
  resource.record_allocation(500);    // Peak = 1000
  resource.record_deallocation(300);  // Used = 700, peak still 1000
  resource.record_allocation(100);    // Used = 800, peak still 1000
  resource.record_allocation(300);    // Used = 1100, peak = 1100

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 1100U);
  EXPECT_EQ(stats.peak_bytes, 1100U);
}

// =============================================================================
// DiskMemoryResource Tests
// =============================================================================

TEST(DiskMemoryResourceTest, DefaultConstruction) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_1";
  config.cleanup_on_destroy = true;

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

TEST(DiskMemoryResourceTest, WithConfig) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_2";
  config.max_bytes = 1024 * 1024 * 100;  // 100 MB
  config.saturation_threshold = 0.85;
  config.cleanup_on_destroy = true;

  DiskMemoryResource resource(config);

  auto stats = resource.stats();
  EXPECT_EQ(stats.total_bytes, 1024U * 1024U * 100U);
  EXPECT_NEAR(resource.saturation_threshold(), 0.85, 0.001);
}

TEST(DiskMemoryResourceTest, GenerateCachePath) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_3";
  config.cleanup_on_destroy = true;

  DiskMemoryResource resource(config);

  auto path1 = resource.generate_cache_path("test");
  auto path2 = resource.generate_cache_path("test");
  auto path3 = resource.generate_cache_path("other");

  // Paths should be unique
  EXPECT_NE(path1, path2);
  EXPECT_NE(path2, path3);

  EXPECT_EQ(path1.parent_path().parent_path(), config.base_path);
}

TEST(DiskMemoryResourceTest, InstancesSharingBasePathDoNotClobberEachOther) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_shared";
  config.cleanup_on_destroy = true;

  DiskMemoryResource survivor(config);
  const auto survivor_file = survivor.generate_cache_path("spill");
  std::ofstream(survivor_file) << "data";

  {
    DiskMemoryResource other(config);
    EXPECT_NE(other.generate_cache_path("spill"), survivor.generate_cache_path("spill"));
  }

  EXPECT_TRUE(std::filesystem::exists(survivor_file));
}

TEST(DiskMemoryResourceTest, RecordFileOperations) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_4";
  config.max_bytes = 10000;
  config.cleanup_on_destroy = true;

  DiskMemoryResource resource(config);

  resource.record_file_created("file1.cache", 1000);
  resource.record_file_created("file2.cache", 500);

  auto stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 1500U);
  EXPECT_EQ(stats.allocation_count, 2U);  // file_count

  resource.record_file_deleted("file1.cache", 1000);

  stats = resource.stats();
  EXPECT_EQ(stats.used_bytes, 500U);
  EXPECT_EQ(stats.allocation_count, 1U);
}

TEST(DiskMemoryResourceTest, Saturation) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_5";
  config.max_bytes = 1000;
  config.saturation_threshold = 0.90;
  config.cleanup_on_destroy = true;

  DiskMemoryResource resource(config);

  EXPECT_FALSE(resource.is_saturated());

  resource.record_file_created("file1.cache", 800);
  EXPECT_FALSE(resource.is_saturated());

  resource.record_file_created("file2.cache", 100);  // Now at 90%
  EXPECT_TRUE(resource.is_saturated());
}

TEST(DiskMemoryResourceTest, CanAllocate) {
  DiskMemoryConfig config;
  config.base_path = std::filesystem::temp_directory_path() / "test_disk_resource_6";
  config.max_bytes = 1000;
  config.cleanup_on_destroy = true;

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

TEST(MemoryResourceManagerTest, Construction) {
  auto cpu = std::make_shared<CPUMemoryResource>();
  auto disk_config = DiskMemoryConfig{
      .base_path = std::filesystem::temp_directory_path() / "test_manager_1",
      .cleanup_on_destroy = true};
  auto disk = std::make_shared<DiskMemoryResource>(disk_config);

  MemoryResourceManager manager(cpu, disk);

  EXPECT_EQ(&manager.cpu(), cpu.get());
  EXPECT_EQ(&manager.disk(), disk.get());
}

TEST(MemoryResourceManagerTest, NullResources) {
  auto cpu = std::make_shared<CPUMemoryResource>();
  auto disk_config = DiskMemoryConfig{
      .base_path = std::filesystem::temp_directory_path() / "test_manager_2",
      .cleanup_on_destroy = true};
  auto disk = std::make_shared<DiskMemoryResource>(disk_config);

  EXPECT_THROW(MemoryResourceManager(nullptr, disk), std::invalid_argument);
  EXPECT_THROW(MemoryResourceManager(cpu, nullptr), std::invalid_argument);
}

TEST(MemoryResourceManagerTest, GetByTier) {
  auto cpu = std::make_shared<CPUMemoryResource>();
  auto disk_config = DiskMemoryConfig{
      .base_path = std::filesystem::temp_directory_path() / "test_manager_3",
      .cleanup_on_destroy = true};
  auto disk = std::make_shared<DiskMemoryResource>(disk_config);

  MemoryResourceManager manager(cpu, disk);

  EXPECT_EQ(manager.get(MemoryTier::CPU).tier(), MemoryTier::CPU);
  EXPECT_EQ(manager.get(MemoryTier::Disk).tier(), MemoryTier::Disk);
}

TEST(MemoryResourceManagerTest, RecommendTier) {
  CPUMemoryConfig cpu_config;
  cpu_config.max_bytes = 1000;
  cpu_config.saturation_threshold = 0.80;
  auto cpu = std::make_shared<CPUMemoryResource>(cpu_config);

  auto disk_config = DiskMemoryConfig{
      .base_path = std::filesystem::temp_directory_path() / "test_manager_4",
      .max_bytes = 10000,
      .cleanup_on_destroy = true};
  auto disk = std::make_shared<DiskMemoryResource>(disk_config);

  MemoryResourceManager manager(cpu, disk);

  // With empty CPU, should recommend CPU
  EXPECT_EQ(manager.recommend_tier(500), MemoryTier::CPU);

  // Allocate to reach saturation (80%)
  cpu->record_allocation(800);  // Now at 80%, which IS saturated (>= threshold)

  // Now CPU is saturated, should recommend Disk
  EXPECT_EQ(manager.recommend_tier(100), MemoryTier::Disk);

  // Large allocation that doesn't fit in CPU should also go to Disk
  EXPECT_EQ(manager.recommend_tier(500), MemoryTier::Disk);
}

TEST(MemoryResourceManagerTest, ShouldSpill) {
  CPUMemoryConfig cpu_config;
  cpu_config.max_bytes = 1000;
  cpu_config.saturation_threshold = 0.80;
  auto cpu = std::make_shared<CPUMemoryResource>(cpu_config);

  auto disk_config = DiskMemoryConfig{
      .base_path = std::filesystem::temp_directory_path() / "test_manager_5",
      .cleanup_on_destroy = true};
  auto disk = std::make_shared<DiskMemoryResource>(disk_config);

  MemoryResourceManager manager(cpu, disk);

  EXPECT_FALSE(manager.should_spill());

  cpu->record_allocation(700);
  EXPECT_FALSE(manager.should_spill());

  cpu->record_allocation(100);  // Now at 80%
  EXPECT_TRUE(manager.should_spill());
}
