// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_PIPELINE_EXECUTOR_HPP_
#define QUEST_TASKFLOW_PIPELINE_EXECUTOR_HPP_

/// @file pipeline_executor.hpp
/// @brief PipelineExecutor — runs a set of Pipelines on a bounded worker
///        pool, replacing Graph::Execute()'s thread-per-operator model
///        (docs/dev/pipeline-execution-plan.md, Stage 4).

#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "quest/cache/data_block.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/operator.hpp"
#include "quest/taskflow/pipeline.hpp"

namespace quest::taskflow {

/// @brief Drives a set of Pipelines to completion on a fixed worker pool.
///
/// One worker drains one pipeline at a time, start to finish (claim a
/// batch, `RunTask`, repeat until the source is exhausted) — intra-pipeline
/// task parallelism is deferred to the task-creator/hint-chain phase (plan
/// §6 Non-goals); this stage only needs inter-pipeline concurrency to be
/// correct. A pipeline becomes schedulable once every one of its
/// `FullBarrierProducers()` has finished; `PipelineBarrierProducers()`
/// never block scheduling — those pipelines may run concurrently, with the
/// shared `BatchHolder` providing the buffered handoff (see
/// `ClaimNextBatch`).
class PipelineExecutor {
 public:
  /// @param num_workers Size of the worker pool. Clamped to at least 1;
  ///        defaults to the hardware's concurrency (also clamped to 1).
  explicit PipelineExecutor(std::size_t num_workers = DefaultWorkerCount());

  ~PipelineExecutor() = default;

  PipelineExecutor(const PipelineExecutor&) = delete;
  PipelineExecutor& operator=(const PipelineExecutor&) = delete;
  PipelineExecutor(PipelineExecutor&&) = delete;
  PipelineExecutor& operator=(PipelineExecutor&&) = delete;

  /// @brief Runs every pipeline to completion.
  ///
  /// Schedules every pipeline with no `FullBarrierProducers()` immediately.
  /// As each pipeline finishes, re-evaluates its dependents and schedules
  /// any that just became runnable — completion is edge-triggered, nothing
  /// polls. On the first error from any pipeline, stops scheduling new
  /// work; pipelines already sitting in the ready queue still drain
  /// ("drain queued work" before "join workers", plan §2.5's teardown
  /// order — only *dependent* scheduling stops), then the pool is joined
  /// before the error is returned.
  ///
  /// @warning ORDERING INVARIANT (until the task-creator phase lands a
  ///          non-blocking claim): a Pipeline-barrier consumer occupies its
  ///          worker while blocked in `Pull()` on the producer's holder. If
  ///          every worker is occupied by such consumers while their
  ///          producers sit unpopped in the ready queue, execution hangs —
  ///          and the stall detector cannot see it (workers are in flight,
  ///          the queue is non-empty). Callers must therefore order
  ///          Pipeline-barrier producers BEFORE their consumers in
  ///          `pipelines`. `MetaPipeline::Build` output satisfies this by
  ///          construction (children always precede parents); hand-assembled
  ///          sets (as in tests/test_pipeline_executor.cpp) must preserve it
  ///          themselves, or provide at least one worker per concurrently
  ///          blocked consumer.
  ///
  /// @param pipelines The pipelines to run, typically `MetaPipeline::
  ///        Build`'s output. Ownership passes to this call; pipelines are
  ///        destroyed when it returns.
  /// @return Status::OK() if every pipeline finished without error, or the
  ///         first error encountered.
  [[nodiscard]] Status Execute(std::vector<PipelinePtr> pipelines);

 private:
  [[nodiscard]] static std::size_t DefaultWorkerCount();

  /// @brief Drains one pipeline start to finish.
  ///
  /// If the pipeline's source is a blocking operator not yet in source
  /// mode (`IsSink() && !IsSource()`) AND this pipeline actually has a
  /// `FullBarrierProducers()` entry (the Sirius "sink of one pipeline,
  /// source of the next" invariant — gated on producers existing, not
  /// just on `IsSink()`, so an operator reporting `IsSink()==true` for
  /// unrelated reasons on a Pipeline-barrier-only pipeline is untouched),
  /// flips it via `FinalizeSink()` first; by the time this runs, every
  /// `FullBarrierProducers()` entry has already finished sinking into it.
  /// Then loops claim-and-`RunTask` until the source is exhausted.
  ///
  /// On every exit path (success or error), marks the pipeline finished
  /// and finishes its sink's output holder(s) — otherwise a
  /// Pipeline-barrier consumer already blocked in `ClaimNextBatch`'s
  /// `Pull()` on this pipeline's output would hang forever once this
  /// pipeline stops producing.
  [[nodiscard]] Status DrainPipeline(Pipeline& pipeline);

  /// @brief Claims the next batch for a pipeline's source operator.
  ///
  /// If `source_op.IsSource()`, claims via `GetNextBatch()` — a real data
  /// source, or a blocking operator already flipped by `FinalizeSink()`.
  /// Otherwise claims by blocking-pulling `source_op`'s own default input
  /// holder (wired by `Graph::AddEdge` — Stage 3's kept deviation: holders
  /// still live on every edge). `Pull()`'s `ErrorCode::Invalid` (queue
  /// finished and empty) is translated to end-of-stream here, the same way
  /// `GetNextBatch()` already represents it — a queue-shaped error becomes
  /// a value, never propagates as a real error.
  ///
  /// @note Only the default (operator-id-named) port is supported; a
  ///       source ending at a partitioned edge is not yet claimable (mirrors
  ///       `Pipeline::PublishOutput`'s matching, already-documented gap).
  /// @return The next batch, `std::nullopt` on end-of-stream, or an error.
  [[nodiscard]] static Result<std::optional<cache::DataBlockPtr>> ClaimNextBatch(
      Operator& source_op);

  /// @brief One worker thread's loop: pop a ready pipeline, drain it,
  /// record any error, then let `OnPipelineFinished` re-evaluate the
  /// runnable set. Exits once nothing is queued and either every pipeline
  /// has finished or scheduling has stopped due to an error.
  void WorkerLoop();

  /// @brief Called once a pipeline's drain loop returns (success or
  /// error). Under `schedule_mutex_`: decrements the outstanding and
  /// in-flight counts, and — unless a prior error already stopped
  /// scheduling — pushes any dependent whose `FullBarrierProducers()` are
  /// now all finished. If that leaves nothing queued and nothing in
  /// flight while pipelines remain unfinished, no further progress is
  /// possible (a Full-barrier cycle); records that as an error and stops
  /// scheduling rather than letting every worker wait forever.
  void OnPipelineFinished(Pipeline* finished_pipeline);

  /// @brief Records the first error seen, if none was recorded yet, and
  /// flips `stop_scheduling_` so no further pipelines get dispatched.
  void RecordError(Status status);

  std::size_t num_workers_;

  // Owns the pipelines for the duration of Execute(); cleared before it
  // returns so callers never observe a half-torn-down state.
  std::vector<PipelinePtr> pipelines_;

  // Precomputed once per Execute() call: producer Pipeline* -> the
  // pipelines that list it as a FullBarrierProducers() entry. Read/written
  // only under schedule_mutex_ once workers are running.
  std::unordered_map<Pipeline*, std::vector<Pipeline*>> full_dependents_;

  std::mutex schedule_mutex_;
  std::condition_variable schedule_cv_;
  std::deque<Pipeline*> ready_queue_;
  std::unordered_set<Pipeline*> scheduled_;
  std::size_t outstanding_{0};
  // Pipelines popped from ready_queue_ but not yet finished (currently
  // being drained by some worker). Distinct from outstanding_ so
  // OnPipelineFinished can detect a permanent stall: nothing queued,
  // nothing in flight, yet outstanding_ > 0 -- no pipeline can ever
  // become runnable again (e.g. a Full-barrier cycle among
  // hand-assembled pipelines; MetaPipeline::Build's cycle detection
  // never runs on input constructed that way).
  std::size_t in_flight_{0};
  bool stop_scheduling_{false};
  Status first_error_{Status::OK()};
};

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_PIPELINE_EXECUTOR_HPP_
