// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#include "quest/taskflow/pipeline_executor.hpp"

#include <algorithm>
#include <string>
#include <thread>
#include <utility>

namespace quest::taskflow {

std::size_t PipelineExecutor::DefaultWorkerCount() {
  auto hardware_threads = std::thread::hardware_concurrency();
  return hardware_threads == 0 ? 1 : static_cast<std::size_t>(hardware_threads);
}

PipelineExecutor::PipelineExecutor(std::size_t num_workers)
    : num_workers_(std::max<std::size_t>(1, num_workers)) {}

Status PipelineExecutor::Execute(std::vector<PipelinePtr> pipelines) {
  if (pipelines.empty()) {
    return {};
  }

  pipelines_ = std::move(pipelines);
  full_dependents_.clear();
  ready_queue_.clear();
  scheduled_.clear();
  stop_scheduling_ = false;
  first_error_ = Status{};
  outstanding_ = pipelines_.size();
  in_flight_ = 0;

  for (const auto& pipeline : pipelines_) {
    for (Pipeline* producer : pipeline->FullBarrierProducers()) {
      full_dependents_[producer].push_back(pipeline.get());
    }
  }

  bool any_runnable_at_start = false;
  {
    std::lock_guard<std::mutex> lock(schedule_mutex_);
    for (const auto& pipeline : pipelines_) {
      if (pipeline->FullBarrierProducers().empty()) {
        scheduled_.insert(pipeline.get());
        ready_queue_.push_back(pipeline.get());
      }
    }
    any_runnable_at_start = !ready_queue_.empty();
  }

  if (!any_runnable_at_start) {
    // MetaPipeline::Build rejects cycles, so an empty starting frontier
    // means a Full-barrier producer chain has no true root: malformed
    // input, not something worth deadlocking a worker pool over. (A
    // cycle among only a *subset* of pipelines, coexisting with at least
    // one independently-runnable pipeline, is instead caught later by
    // OnPipelineFinished's stall check.)
    pipelines_.clear();
    scheduled_.clear();
    return Error::Invalid(
        "PipelineExecutor: no pipeline is runnable at start (a "
        "Full-barrier producer chain has no root)");
  }

  {
    std::vector<std::jthread> workers;
    workers.reserve(num_workers_);
    for (std::size_t i = 0; i < num_workers_; ++i) {
      workers.emplace_back([this] { WorkerLoop(); });
    }
    // jthreads join here, on scope exit, before pipelines_ is torn down.
  }

  Status result = first_error_;
  pipelines_.clear();
  scheduled_.clear();
  full_dependents_.clear();
  return result;
}

void PipelineExecutor::WorkerLoop() {
  while (true) {
    Pipeline* pipeline = nullptr;
    {
      std::unique_lock<std::mutex> lock(schedule_mutex_);
      schedule_cv_.wait(lock, [this] {
        return !ready_queue_.empty() || outstanding_ == 0 || stop_scheduling_;
      });
      if (ready_queue_.empty()) {
        return;  // Nothing left to do, or scheduling stopped after an error.
      }
      pipeline = ready_queue_.front();
      ready_queue_.pop_front();
      ++in_flight_;
    }

    auto status = DrainPipeline(*pipeline);
    if (!status.has_value()) {
      RecordError(std::move(status));
    }
    OnPipelineFinished(pipeline);
  }
}

void PipelineExecutor::OnPipelineFinished(Pipeline* finished_pipeline) {
  std::lock_guard<std::mutex> lock(schedule_mutex_);
  --outstanding_;
  --in_flight_;

  if (!stop_scheduling_) {
    auto dependents_it = full_dependents_.find(finished_pipeline);
    if (dependents_it != full_dependents_.end()) {
      for (Pipeline* dependent : dependents_it->second) {
        bool all_full_producers_finished =
            std::ranges::all_of(dependent->FullBarrierProducers(),
                                 [](Pipeline* producer) { return producer->IsFinished(); });
        if (all_full_producers_finished && scheduled_.insert(dependent).second) {
          ready_queue_.push_back(dependent);
        }
      }
    }

    // Nothing queued, nothing running, but pipelines remain unfinished:
    // no pipeline can ever become runnable again. Left unchecked, every
    // worker would park on schedule_cv_ forever (the wait predicate is
    // never satisfied once outstanding_ can no longer reach 0). This is
    // the "Full-barrier cycle" case the initial ready_queue_ check in
    // Execute() doesn't cover: at least one pipeline WAS runnable at
    // start (so that check passed), but a disjoint subset of pipelines
    // depends only on each other in a cycle.
    if (ready_queue_.empty() && in_flight_ == 0 && outstanding_ > 0) {
      if (first_error_.has_value()) {
        first_error_ = Error::Invalid(
            "PipelineExecutor: deadlocked -- no pipeline is runnable but "
            "some have not finished (a Full-barrier cycle among a subset "
            "of pipelines?)");
      }
      stop_scheduling_ = true;
    }
  }

  schedule_cv_.notify_all();
}

void PipelineExecutor::RecordError(Status status) {
  std::lock_guard<std::mutex> lock(schedule_mutex_);
  if (first_error_.has_value()) {
    first_error_ = std::move(status);
  }
  stop_scheduling_ = true;
  schedule_cv_.notify_all();
}

Status PipelineExecutor::DrainPipeline(Pipeline& pipeline) {
  Operator* source_op = pipeline.source();
  if (source_op == nullptr) {
    return Error::Invalid("PipelineExecutor: pipeline has no source operator");
  }

  // Whatever happens below, this pipeline is done running once this
  // function returns -- mark it finished unconditionally (success or
  // error) so it never sits in a permanently ambiguous state, and finish
  // its sink's output holder(s) on every exit path. Without the latter, a
  // Pipeline-barrier consumer already blocked in ClaimNextBatch's
  // holder->Pull() would hang forever once this pipeline stops producing
  // -- Pull() only wakes on new data or Finish(), and nothing else ever
  // signals it. An accumulating sink's output is not this pipeline's to
  // finish: it publishes nothing there, and a hash join's probe pipeline
  // still streams into it.
  struct FinishOnExit {
    Pipeline& pipeline;
    ~FinishOnExit() {
      if (Operator* sink_op = pipeline.sink(); sink_op != nullptr && !pipeline.SinkAccumulates()) {
        sink_op->output().Finish();
      }
      pipeline.MarkFinished();
    }
  } finish_on_exit{pipeline};

  // A role-based source (IsSource(), including a blocking operator its
  // producer pipeline finalized) produced the claimed batch itself, so RunTask must
  // skip it (start_index 1). A holder-fed source did NOT see the batch --
  // it arrived from the cross-pipeline holder -- so its own ProcessBatch
  // is the first stop (start_index 0). Skipping it would silently drop
  // the first operator's transform in any multi-operator pipeline that
  // begins at an explicit Full/partitioned break.
  const std::size_t run_start_index = source_op->IsSource() ? 1 : 0;

  // An ordered list of input holders drained sequentially (one to EOS, then
  // the next): FOR_EACH partitioned lanes (input_0…N-1, Phase 2.6 E3) OR a
  // multi-input operator's declared named ports (Union's input_a/input_b,
  // Phase 2.7 F2). Mutually exclusive — a source uses numeric lanes or named
  // ports, never both — so prefer lanes and fall back to named ports.
  auto ordered_inputs = pipeline.InputLanes();
  if (ordered_inputs.empty()) {
    ordered_inputs = pipeline.InputPorts();
  }
  std::size_t input_cursor = 0;

  while (true) {
    // Single-pipeline early stop (e.g. Limit remaining == 0): do not claim
    // further source batches. Checked before each claim so Limit(0) never
    // pulls. FinishOnExit still finishes sink outputs.
    // Multi-pipeline / cross-barrier cancellation is out of scope.
    bool stop_source = false;
    for (const Operator* op : pipeline.operators()) {
      if (op != nullptr && op->ShouldStopSource()) {
        stop_source = true;
        break;
      }
    }
    if (stop_source) {
      break;
    }

    std::optional<cache::DataBlockPtr> batch;
    {
      // Load-bearing (plan §2.5): claim input -> construct task ->
      // MarkTaskCreated() as one atomic step under this pipeline's own
      // status_lock(), so a completion check can never observe an empty
      // source and balanced counters while a claimed batch exists but
      // isn't counted yet.
      //
      // Known deferred scope limitation: when ClaimNextBatch takes the
      // holder-pull branch, this lock is held across an unbounded
      // Pull(). Harmless today because Stage 4 is strictly one active
      // drain per pipeline (nothing else ever contends for THIS
      // pipeline's status_lock() while it runs), but it will need to
      // narrow to just the accounting step once intra-pipeline task
      // parallelism (plan §6 Non-goals) needs this same lock to run a
      // concurrent completion check while a sibling task sits parked in
      // Pull().
      std::lock_guard<std::mutex> claim_lock(pipeline.status_lock());
      auto claimed = ClaimNextBatch(*source_op, ordered_inputs, input_cursor);
      if (!claimed.has_value()) {
        return std::unexpected(claimed.error());
      }
      if (!claimed.value().has_value()) {
        break;  // Source exhausted.
      }
      batch = std::move(claimed.value());
      pipeline.MarkTaskCreated();
    }

    auto run_status = pipeline.RunTask(std::move(*batch), run_start_index);
    pipeline.MarkTaskCompleted();
    if (!run_status.has_value()) {
      return run_status;
    }
  }

  // Every task of this pipeline has run (this worker drained them all), and
  // FinishOnExit marks it finished only after this returns, so dependents
  // that consume the sink's accumulated state are scheduled after it is
  // finalized.
  if (pipeline.SinkAccumulates()) {
    return pipeline.sink()->FinalizeSink();
  }
  return {};
}

Result<std::optional<cache::DataBlockPtr>> PipelineExecutor::PullFromHolder(
    cache::IBatchHolder& holder) {
  return holder.Pull();  // Blocks until data arrives or the producer finishes.
}

Result<std::optional<cache::DataBlockPtr>> PipelineExecutor::ClaimNextBatch(
    Operator& source_op,
    const std::vector<std::shared_ptr<cache::IBatchHolder>>& ordered_inputs,
    std::size_t& input_cursor) {
  if (source_op.IsSource()) {
    return source_op.GetNextBatch();
  }

  // Ordered multi-input drain: FOR_EACH lanes (input_0…N-1) or a multi-input
  // operator's declared named ports (input_a, input_b). Drain the current
  // holder to EOS, then advance — per-input order preserved (merge-friendly
  // for lanes, deterministic UNION ALL for named ports).
  if (!ordered_inputs.empty()) {
    while (input_cursor < ordered_inputs.size()) {
      auto input_result = PullFromHolder(*ordered_inputs[input_cursor]);
      if (!input_result.has_value()) {
        return std::unexpected(input_result.error());
      }
      if (input_result.value().has_value()) {
        return input_result;  // data from the current input
      }
      ++input_cursor;  // input exhausted — move to the next
    }
    return std::optional<cache::DataBlockPtr>{std::nullopt};  // all inputs done
  }

  auto default_port = std::to_string(source_op.id());
  if (!source_op.input().has_port(default_port)) {
    return Err(ErrorCode::Invalid,
               "PipelineExecutor: pipeline source has neither a source "
               "role nor a wired default input holder to claim from");
  }

  auto holder = source_op.InputCache();
  if (!holder) {
    return std::unexpected(holder.error());
  }
  if (!*holder) {
    return Err(ErrorCode::Invalid,
               "PipelineExecutor: pipeline source's default input port "
               "has no batch holder");
  }

  return PullFromHolder(**holder);
}

}  // namespace quest::taskflow
