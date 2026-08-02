#ifndef QUEST_QUEST_HPP_
#define QUEST_QUEST_HPP_

/// @file quest.hpp
/// @brief Convenience umbrella header for the QUEST library.

#include "quest/result.hpp"
#include "quest/memory/memory_resource.hpp"
#include "quest/cache/blocking_queue.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/cache/batch_holder.hpp"
#include "quest/taskflow/operator_type.hpp"
#include "quest/taskflow/port.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/edge_ref.hpp"
#include "quest/taskflow/graph.hpp"
#include "quest/taskflow/graph_builder.hpp"

namespace quest {

constexpr std::string_view kVersion = "0.1.0";

[[nodiscard]] inline std::shared_ptr<memory::MemoryResourceManager>
make_default_memory_manager() {
    auto cpu = std::make_shared<memory::CPUMemoryResource>();
    auto disk = std::make_shared<memory::DiskMemoryResource>();
    return std::make_shared<memory::MemoryResourceManager>(
        std::move(cpu), std::move(disk));
}

}  // namespace quest

#endif  // QUEST_QUEST_HPP_
