// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/prepared_query.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "quest/taskflow/pipeline_executor.hpp"

namespace quest::taskflow {

PreparedQuery::PreparedQuery(std::vector<PreparedEdge> edges, std::vector<PipelinePtr> pipelines)
    : edges_(std::move(edges)),
      pipelines_(std::move(pipelines)),
      executor_(std::make_unique<PipelineExecutor>()) {}

PreparedQuery::~PreparedQuery() = default;

PreparedQuery::PreparedQuery(PreparedQuery&&) noexcept = default;
PreparedQuery& PreparedQuery::operator=(PreparedQuery&&) noexcept = default;

Status PreparedQuery::Execute() {
  // Single-shot: pipelines_ moves into the executor. A second call must be
  // a loud error, not a silent OK for a query that didn't run (review P2).
  if (executed_) {
    return Error::Invalid(
        "PreparedQuery::Execute: already executed — re-run by calling "
        "Graph::Prepare() again");
  }
  executed_ = true;
  if (pipelines_.empty()) {
    return {};
  }
  return executor_->Execute(std::move(pipelines_));
}

std::vector<EdgeInfo> PreparedQuery::Edges() const {
  std::vector<EdgeInfo> infos;
  infos.reserve(edges_.size());
  for (const auto& edge : edges_) {
    infos.push_back(edge.info);
  }
  return infos;
}

std::size_t PreparedQuery::holder_count() const noexcept {
  std::size_t count = 0;
  for (const auto& edge : edges_) {
    count += edge.holders.size();
  }
  return count;
}

std::vector<std::shared_ptr<cache::IBatchHolder>> PreparedQuery::holders() const {
  std::vector<std::shared_ptr<cache::IBatchHolder>> flat;
  flat.reserve(holder_count());
  for (const auto& edge : edges_) {
    flat.insert(flat.end(), edge.holders.begin(), edge.holders.end());
  }
  return flat;
}

PreparedEdge* PreparedQuery::FindEdge(const EdgeRef& ref) {
  for (auto& edge : edges_) {
    if (edge.info.ref == ref) {
      return &edge;
    }
  }
  // Also match when caller uses "" for default port and the resolved port is
  // the target-id string (GraphBuilder resolves defaults at Build).
  const std::string default_port = std::to_string(ref.target_id);
  for (auto& edge : edges_) {
    if (edge.info.ref.source_id == ref.source_id && edge.info.ref.target_id == ref.target_id &&
        (ref.target_port.empty()
             ? (edge.info.ref.target_port.empty() || edge.info.ref.target_port == default_port)
             : edge.info.ref.target_port == ref.target_port)) {
      return &edge;
    }
  }
  return nullptr;
}

const PreparedEdge* PreparedQuery::FindEdge(const EdgeRef& ref) const {
  return const_cast<PreparedQuery*>(this)->FindEdge(ref);
}

Status PreparedQuery::SetFlowControl(const EdgeRef& ref, std::size_t max_batches,
                                     std::size_t max_bytes) {
  auto* edge = FindEdge(ref);
  if (edge == nullptr) {
    return Error::Invalid(std::format("PreparedQuery::SetFlowControl: no edge {}:{}:{}",
                                       ref.source_id, ref.target_id, ref.target_port));
  }

  edge->info.config.flow_control_batches_threshold = max_batches;
  edge->info.config.flow_control_bytes_threshold = max_bytes;

  for (auto& holder : edge->holders) {
    holder->SetFlowControl(max_batches, max_bytes);
  }
  return {};
}

Status PreparedQuery::SetSpillPolicy(const EdgeRef& ref, SpillPolicy policy) {
  auto* edge = FindEdge(ref);
  if (edge == nullptr) {
    return Error::Invalid(std::format("PreparedQuery::SetSpillPolicy: no edge {}:{}:{}",
                                       ref.source_id, ref.target_id, ref.target_port));
  }

  edge->info.config.spill_policy = policy;
  const bool enable_auto_spill = (policy != SpillPolicy::Never);
  for (auto& holder : edge->holders) {
    holder->SetSpillPolicy(enable_auto_spill);
  }
  return {};
}

Status PreparedQuery::ScaleFlowControl(double factor) {
  if (!(factor > 0.0) || !std::isfinite(factor)) {
    return Error::Invalid("PreparedQuery::ScaleFlowControl: factor must be > 0");
  }

  for (auto& edge : edges_) {
    auto& cfg = edge.info.config;
    auto scale = [&](std::size_t value) -> std::size_t {
      if (value == kUnlimited) {
        return kUnlimited;
      }
      const double scaled = static_cast<double>(value) * factor;
      if (scaled >= static_cast<double>(kUnlimited)) {
        return kUnlimited;
      }
      return std::max<std::size_t>(1, static_cast<std::size_t>(scaled));
    };

    const std::size_t new_batches = scale(cfg.flow_control_batches_threshold);
    const std::size_t new_bytes =
        (cfg.flow_control_bytes_threshold == kUnlimited)
            ? kUnlimited
            : std::max<std::size_t>(
                  1, static_cast<std::size_t>(
                         static_cast<double>(cfg.flow_control_bytes_threshold) * factor));

    cfg.flow_control_batches_threshold = new_batches;
    cfg.flow_control_bytes_threshold = new_bytes;

    for (auto& holder : edge.holders) {
      holder->SetFlowControl(new_batches, new_bytes);
    }
  }
  return {};
}

Status PreparedQuery::SetMaxInFlightTasks(std::size_t /*pipeline_id*/, std::size_t /*n*/) {
  return Error::Invalid(
      "PreparedQuery::SetMaxInFlightTasks is reserved until phase 3 "
      "(task-creator admission)");
}

}  // namespace quest::taskflow
