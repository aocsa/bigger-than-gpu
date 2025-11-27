#pragma once

/// @file quest.hpp
/// @brief Main include header for Bigger Than GPU library
/// @details This is a CPU/Disk focused reimplementation of the BlazingSQL
///          "Bigger than GPU" architecture. Data tables reside in CPU memory,
///          with disk used as a cache when RAM is exhausted.

// Result and Status types
#include "quest/result.hpp"

// Memory management
#include "quest/memory/memory_resource.hpp"

// Cache system
#include "quest/cache/blocking_queue.hpp"
#include "quest/cache/cache_data.hpp"
#include "quest/cache/batch_holder.hpp"

// Taskflow execution
#include "quest/taskflow/operator_type.hpp"
#include "quest/taskflow/port.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/kpair.hpp"
#include "quest/taskflow/graph.hpp"

namespace quest {

/// @brief Library version
constexpr std::string_view kVersion = "0.1.0";

/// @brief Create a default memory resource manager
[[nodiscard]] inline std::shared_ptr<memory::MemoryResourceManager> 
make_default_memory_manager() {
    auto cpu = std::make_shared<memory::CPUMemoryResource>();
    auto disk = std::make_shared<memory::DiskMemoryResource>();
    return std::make_shared<memory::MemoryResourceManager>(
        std::move(cpu), std::move(disk));
}

}  // namespace quest
