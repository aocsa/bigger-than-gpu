// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_EDGE_REF_HPP_
#define QUEST_TASKFLOW_EDGE_REF_HPP_

/// @file edge_ref.hpp
/// @brief Edge identity and introspection types for runtime tuning (Phase 2.5 D5).
///
/// EdgeRef addresses an edge after construction (by endpoints + port). EdgeInfo
/// is the introspection view returned by Graph::Edges() / PreparedQuery::Edges().

#include <cstdint>
#include <string>

#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/operator_type.hpp"

namespace quest::taskflow {

/// @brief Stable identity for an edge after Build()/Prepare().
///
/// `target_port` is "" for the default input port. After GraphBuilder::Build(),
/// default ports are resolved to the target operator-id string — prefer the
/// values returned by Edges() when constructing refs by hand.
struct EdgeRef {
  std::int32_t source_id{0};
  std::int32_t target_id{0};
  std::string target_port;

  [[nodiscard]] bool operator==(const EdgeRef& other) const noexcept {
    return source_id == other.source_id && target_id == other.target_id &&
           target_port == other.target_port;
  }
};

/// @brief Introspection snapshot of one edge (Built or Prepared view).
struct EdgeInfo {
  EdgeRef ref;
  std::string name;  ///< Author-given via Named(), or generated for synthesized edges.
  bool synthesized{false};
  EdgeConfig config;  ///< Resolved so far (author+contract on Built; +policies on Prepared).
  OperatorType source_type{OperatorType::Unknown};
  OperatorType target_type{OperatorType::Unknown};
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_EDGE_REF_HPP_
