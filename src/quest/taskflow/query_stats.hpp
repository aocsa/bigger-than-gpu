// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_QUERY_STATS_HPP_
#define QUEST_TASKFLOW_QUERY_STATS_HPP_

/// @file query_stats.hpp
/// @brief Statistics and hardware descriptors passed to Graph::Prepare().
///
/// Kept in a leaf header (no Graph / PreparedQuery includes) so the
/// Prepare() surface can be declared without include cycles. Phase 4
/// reservation sizing and Phase 2.5 D5 tuning policies share this
/// vocabulary.

#include <cstddef>

namespace quest::taskflow {

/// @brief Estimated query input sizes (Parquet footers / DataSource metadata).
struct QueryStats {
  std::size_t estimated_rows{0};
  std::size_t estimated_bytes{0};
  std::size_t row_group_count{0};
};

/// @brief Probed hardware resources for a Prepare() call.
struct HardwareInfo {
  std::size_t cores{0};
  std::size_t ram_bytes{0};
  std::size_t disk_free_bytes{0};
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_QUERY_STATS_HPP_
