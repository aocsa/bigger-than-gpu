// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/pipeline.hpp"

#include <utility>

namespace quest::taskflow {

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
      return Status::OK();
    }
  }
  return PublishOutput(std::move(batch));
}

Status Pipeline::PublishOutput(cache::DataBlockPtr batch) const {
  Operator* sink_op = sink();
  if (sink_op == nullptr) {
    return Status::Invalid("Pipeline::PublishOutput: pipeline has no sink");
  }

  if (sink_op->IsSink()) {
    // Blocking operator: accumulate state. FinalizeSink() (called once the
    // producer pipeline is drained) flips it into source mode.
    return sink_op->Sink(std::move(batch));
  }

  // Streaming operator ending this pipeline only because of an edge
  // barrier (no blocking op involved): it still has to run its own
  // transform before the result crosses into the next pipeline's holder.
  auto processed = sink_op->ProcessBatch(std::move(batch));
  if (!processed.has_value()) {
    return std::unexpected(processed.error());
  }
  return sink_op->AddToOutput(std::move(processed.value()));
}

}  // namespace quest::taskflow
