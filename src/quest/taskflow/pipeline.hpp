// Copyright 2024 Quest Authors
// SPDX-License-Identifier: Apache-2.0

#ifndef QUEST_TASKFLOW_PIPELINE_HPP_
#define QUEST_TASKFLOW_PIPELINE_HPP_

/// @file pipeline.hpp
/// @brief Pipeline — an ordered operator list executed as one forward push
///        pass per batch (docs/dev/pipeline-execution-plan.md, Stage 3).
///
/// A Pipeline mirrors Sirius's `sirius_pipeline`: `operators()` holds EVERY
/// operator between a source and a sink, source included. `RunTask` drives
/// one batch through the streaming operators in plan order and hands the
/// final batch to the sink role via `PublishOutput`. Blocking operators
/// appear as the sink of one pipeline and the source of the pipeline that
/// consumes their accumulated state (`MetaPipeline::Build` wires this up).

#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>
#include <vector>

#include "quest/cache/batch_holder.hpp"
#include "quest/cache/data_block.hpp"
#include "quest/result.hpp"
#include "quest/taskflow/edge_config.hpp"
#include "quest/taskflow/operator.hpp"

namespace quest::taskflow {

// =============================================================================
// Pipeline
// =============================================================================

/// @brief An ordered chain of operators executed as one forward pass.
///
/// `operators()[0]` is the pipeline's source; `operators().back()` is its
/// sink (they are the same operator for a single-operator pipeline). Both
/// roles are always present in the list, matching Sirius's invariant that a
/// pipeline's operator list contains every operator, source and sink
/// included.
///
/// Non-copyable, non-movable: `status_lock()` guards the task-accounting
/// discipline Stage 4's executor depends on (see
/// tasks/notes/pipelines.md's "completion predicate" section), so a
/// Pipeline must be heap-allocated and referenced by pointer.
class Pipeline {
 public:
  explicit Pipeline(std::size_t pipeline_id) noexcept : pipeline_id_(pipeline_id) {}

  ~Pipeline() = default;

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  Pipeline(Pipeline&&) = delete;
  Pipeline& operator=(Pipeline&&) = delete;

  // ===========================================================================
  // Identity
  // ===========================================================================

  [[nodiscard]] std::size_t id() const noexcept { return pipeline_id_; }

  // ===========================================================================
  // Structure (built by MetaPipeline::Build via upstream prepend)
  // ===========================================================================

  /// @brief Inserts an operator at the front of the operator list.
  ///
  /// `MetaPipeline::Build` walks upstream from a sink, so operators arrive
  /// in reverse plan order; prepending restores forward (source-to-sink)
  /// order without a second pass.
  void PrependOperator(Operator* new_front) { operators_.insert(operators_.begin(), new_front); }

  [[nodiscard]] const std::vector<Operator*>& operators() const noexcept { return operators_; }

  /// @brief The pipeline's source operator (front of the list).
  /// @return nullptr if the pipeline has no operators yet.
  [[nodiscard]] Operator* source() const noexcept {
    return operators_.empty() ? nullptr : operators_.front();
  }

  /// @brief The pipeline's sink operator (back of the list).
  /// @return nullptr if the pipeline has no operators yet.
  [[nodiscard]] Operator* sink() const noexcept {
    return operators_.empty() ? nullptr : operators_.back();
  }

  // ===========================================================================
  // Cross-pipeline dependencies
  // ===========================================================================

  /// @brief Records that `producer` feeds this pipeline across the given
  /// barrier.
  ///
  /// `Full`-barrier producers are the pipelines this pipeline may not start
  /// consuming from until they finish (Stage 4's runnable-set check).
  /// `Pipeline`-barrier producers may run concurrently with this one.
  ///
  /// @param producer The upstream pipeline (non-owning; owned by the
  ///        vector `MetaPipeline::Build` returns).
  /// @param barrier The barrier semantics of the edge connecting them.
  void AddProducer(Pipeline* producer, BarrierType barrier) {
    if (barrier == BarrierType::Full) {
      full_barrier_producers_.push_back(producer);
    } else {
      pipeline_barrier_producers_.push_back(producer);
    }
  }

  [[nodiscard]] const std::vector<Pipeline*>& FullBarrierProducers() const noexcept {
    return full_barrier_producers_;
  }

  [[nodiscard]] const std::vector<Pipeline*>& PipelineBarrierProducers() const noexcept {
    return pipeline_barrier_producers_;
  }

  // ===========================================================================
  // Task accounting (Stage 4 consumes this; built now per plan §2.5 so the
  // executor phase is additive rather than a redesign)
  // ===========================================================================

  /// @brief Records that a task claimed input and began running.
  ///
  /// Must be called while holding `status_lock()` together with the input
  /// claim that produced the task — see tasks/notes/pipelines.md for why
  /// this ordering is load-bearing. The counter itself is atomic (like
  /// Sirius's tasks_created/tasks_completed) so completion-side reads
  /// don't require the lock.
  void MarkTaskCreated() noexcept { tasks_created_.fetch_add(1); }

  /// @brief Records that a running task finished (successfully or not).
  ///
  /// Unlike MarkTaskCreated(), this may be called without status_lock()
  /// (Sirius increments from the task destructor on a worker thread); the
  /// executor re-evaluates the completion predicate under the lock after.
  void MarkTaskCompleted() noexcept { tasks_completed_.fetch_add(1); }

  [[nodiscard]] std::size_t tasks_created() const noexcept {
    return tasks_created_.load();
  }
  [[nodiscard]] std::size_t tasks_completed() const noexcept {
    return tasks_completed_.load();
  }

  /// @brief Whether this pipeline has been marked finished.
  ///
  /// A thin flag rather than the full completion predicate: Stage 4 derives
  /// "finished" from source exhaustion + empty holders + balanced task
  /// counters and calls MarkFinished() once that predicate holds.
  [[nodiscard]] bool IsFinished() const noexcept { return finished_.load(); }

  void MarkFinished() noexcept { finished_.store(true); }

  /// @brief The lock guarding this pipeline's task-accounting sequence.
  ///
  /// Load-bearing (plan §2.5): claim input -> construct task ->
  /// MarkTaskCreated() must happen atomically under this lock, or a
  /// completion check can observe empty ports and balanced counters while
  /// a task exists conceptually but isn't counted yet.
  [[nodiscard]] std::mutex& status_lock() noexcept { return status_lock_; }

  // ===========================================================================
  // Execution (the push loop)
  // ===========================================================================

  /// @brief Drives one batch forward through the pipeline's operators.
  ///
  /// Runs `ProcessBatch` on `operators()[start_index .. size-2]` in plan
  /// order, then hands the result to `PublishOutput`. `start_index`
  /// defaults to 1 because `operators()[0]` (the source) already produced
  /// this batch and should not see it again; a future OOM-reschedule path
  /// (Sirius's `compute_task` start_index) can resume mid-pipeline by
  /// passing a larger value.
  ///
  /// @param batch The batch to push through the pipeline.
  /// @param start_index The first operator index to run ProcessBatch on.
  /// @return Status::OK() on success (including "a streaming operator
  ///         consumed the batch to zero rows"), or the first error
  ///         encountered.
  [[nodiscard]] Status RunTask(cache::DataBlockPtr batch, std::size_t start_index = 1);

  // ===========================================================================
  // FOR_EACH lanes (BlazingSQL partitioned edges — Phase 2.6 E3)
  // ===========================================================================

  /// @brief The sink's `output_0…N-1` lane holders, numerically ordered.
  /// Empty when the pipeline doesn't end at a partitioned edge.
  [[nodiscard]] std::vector<std::shared_ptr<cache::IBatchHolder>> OutputLanes() const;

  /// @brief The source's `input_0…N-1` lane holders, numerically ordered.
  /// Empty when the pipeline doesn't start at a partitioned edge.
  [[nodiscard]] std::vector<std::shared_ptr<cache::IBatchHolder>> InputLanes() const;

  /// @brief The source's declared named-port holders, in `DeclaredInputPorts()`
  /// order. Empty unless the source is a multi-input operator (e.g. Union's
  /// `input_a`/`input_b`). The executor drains these sequentially — the named
  /// analog of FOR_EACH lanes, giving deterministic UNION ALL concatenation.
  [[nodiscard]] std::vector<std::shared_ptr<cache::IBatchHolder>> InputPorts() const;

 private:
  /// @brief Hands the final batch of a push loop to the sink role.
  ///
  /// If `sink()` is a blocking operator (`IsSink()`), it accumulates state
  /// via `Sink()`. Otherwise `sink()` is a streaming operator that ends
  /// this pipeline only because of an edge barrier: it runs its own
  /// `ProcessBatch`, then the result is published — round-robin across
  /// `output_0…N-1` lanes when the edge is partitioned (FOR_EACH), else to
  /// `AddToOutput()`'s default (operator-id-named) port.
  [[nodiscard]] Status PublishOutput(cache::DataBlockPtr batch);

  std::size_t pipeline_id_;
  std::vector<Operator*> operators_;

  /// Round-robin cursor for FOR_EACH lane publication.
  std::atomic<std::size_t> publish_lane_{0};

  std::vector<Pipeline*> full_barrier_producers_;
  std::vector<Pipeline*> pipeline_barrier_producers_;

  // Atomic (not merely lock-guarded) to match the Sirius pattern: creation
  // increments happen under status_lock_ with the input claim, but
  // completion increments and predicate reads happen on worker threads.
  std::atomic<std::size_t> tasks_created_{0};
  std::atomic<std::size_t> tasks_completed_{0};
  std::atomic<bool> finished_{false};
  std::mutex status_lock_;
};

/// @brief Owning handle for a Pipeline (see the class comment for why
/// Pipeline itself cannot be stored by value).
using PipelinePtr = std::unique_ptr<Pipeline>;

}  // namespace quest::taskflow

#endif  // QUEST_TASKFLOW_PIPELINE_HPP_
