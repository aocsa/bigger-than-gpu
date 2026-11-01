// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/pipeline.hpp"

#include <algorithm>
#include <charconv>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace quest::taskflow {

namespace {

/// @brief Collect FOR_EACH lane holders from a port, numerically ordered.
///
/// BlazingSQL lane convention: `output_0 … output_{N-1}` / `input_0 …`.
/// Numeric parse (not lexicographic map order) so lane 10 sorts after lane 2.
std::vector<std::shared_ptr<cache::IBatchHolder>> CollectLanes(Port& port,
                                                               std::string_view prefix) {
  std::vector<std::pair<std::size_t, std::shared_ptr<cache::IBatchHolder>>> indexed;
  for (auto& [name, holder] : port) {
    if (!holder || !name.starts_with(prefix)) {
      continue;
    }
    std::size_t lane_index = 0;
    const char* first = name.data() + prefix.size();
    const char* last = name.data() + name.size();
    auto [ptr, ec] = std::from_chars(first, last, lane_index);
    if (ec != std::errc{} || ptr != last) {
      continue;  // not a pure lane name
    }
    indexed.emplace_back(lane_index, holder);
  }
  std::ranges::sort(indexed, {}, &decltype(indexed)::value_type::first);

  std::vector<std::shared_ptr<cache::IBatchHolder>> lanes;
  lanes.reserve(indexed.size());
  for (auto& [ignored, holder] : indexed) {
    lanes.push_back(std::move(holder));
  }
  return lanes;
}

}  // namespace

std::vector<std::shared_ptr<cache::IBatchHolder>> Pipeline::OutputLanes() const {
  Operator* sink_op = sink();
  if (sink_op == nullptr) {
    return {};
  }
  return CollectLanes(sink_op->output(), "output_");
}

std::vector<std::shared_ptr<cache::IBatchHolder>> Pipeline::InputLanes() const {
  Operator* source_op = source();
  if (source_op == nullptr) {
    return {};
  }
  return CollectLanes(source_op->input(), "input_");
}

std::vector<std::shared_ptr<cache::IBatchHolder>> Pipeline::InputPorts() const {
  Operator* source_op = source();
  if (source_op == nullptr) {
    return {};
  }
  std::vector<std::shared_ptr<cache::IBatchHolder>> ports;
  Port& input = source_op->input();
  for (std::string_view port_name : source_op->DeclaredInputPorts()) {
    if (auto holder = input.get(std::string(port_name))) {
      ports.push_back(*holder);
    }
  }
  return ports;
}

Status Pipeline::RunTask(cache::DataBlockPtr batch, std::size_t start_index) {
  // operators_[1 .. size-2] are the streaming operators: operators_[0]
  // already produced this batch (the source role), and operators_.back()
  // is handled by PublishOutput (the sink role).
  for (std::size_t i = start_index; i + 1 < operators_.size(); ++i) {
    auto result = operators_[i]->ProcessBatch(std::move(batch));
    if (!result.has_value()) {
      return std::unexpected(result.error());
    }
    batch = std::move(result.value());
    if (batch == nullptr || batch->num_rows() == 0) {
      // A streaming operator (e.g. a filter) consumed the batch entirely.
      // Nothing to publish; this is not an error.
      return {};
    }
  }
  return PublishOutput(std::move(batch));
}

Status Pipeline::PublishOutput(cache::DataBlockPtr batch) {
  Operator* sink_op = sink();
  if (sink_op == nullptr) {
    return Error::Invalid("Pipeline::PublishOutput: pipeline has no sink");
  }

  if (SinkAccumulates()) {
    // Blocking operator: accumulate state. FinalizeSink() (called once this
    // pipeline is drained) flips it into source mode.
    return sink_op->Sink(std::move(batch));
  }

  // Streaming operator ending this pipeline only because of an edge
  // barrier (no blocking op involved): it still has to run its own
  // transform before the result crosses into the next pipeline's holder.
  auto processed = sink_op->ProcessBatch(std::move(batch));
  if (!processed.has_value()) {
    return std::unexpected(processed.error());
  }

  // Nothing to publish: a streaming sink can empty a batch (Limit exhausted,
  // filter dropped every row). Mirror RunTask's 0-row short-circuit so a
  // downstream holder never receives an empty (0-column) batch.
  if (processed.value() == nullptr || processed.value()->num_rows() == 0) {
    return {};
  }

  // FOR_EACH partitioned edge: round-robin across output_0…N-1 lanes
  // (BlazingSQL's fan-out shape; real partition operators pick lanes by
  // hash/range themselves in phase 6 — round-robin is the generic default).
  auto lanes = OutputLanes();
  if (!lanes.empty()) {
    const std::size_t lane = publish_lane_.fetch_add(1) % lanes.size();
    return lanes[lane]->Add(std::move(processed.value()));
  }

  return sink_op->AddToOutput(std::move(processed.value()));
}

}  // namespace quest::taskflow
