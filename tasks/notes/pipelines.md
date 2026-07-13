# How one batch crosses a pipeline break in Sirius

> Stage 0 note for [docs/dev/pipeline-execution-plan.md](../../docs/dev/pipeline-execution-plan.md).
> Source: sirius @ `docs-tutorial`, files read: `sirius_meta_pipeline.cpp`,
> `sirius_pipeline.cpp`, `gpu_pipeline_task.cpp`, `sirius_physical_operator.hpp/.cpp`.

## Where a pipeline ends

A `sirius_meta_pipeline` is a set of pipelines sharing one sink. `build()` walks
top-down from the sink via `op.build_pipelines()`. The generic recursion
(`sirius_physical_operator.cpp:145-172`):

- op `is_sink()` (blocking) → the blocking op becomes the **source of the
  current pipeline** (`state.set_pipeline_source(current, *this)`), then it
  spawns a **child meta-pipeline whose sink is this op** and recurses into
  `children[0]`. `create_child_meta_pipeline` records
  `current.add_dependency(child->get_base_pipeline())` — build side finishes
  before the consumer runs.
- non-sink with children → streaming operator, append and keep walking.
- non-sink, no children → it is the pipeline source (scan).

Joins (`sirius_physical_hash_join.cpp:355-403`): RHS build side is built FIRST
as a child meta-pipeline sinking into the join; then the LHS probe side recurses
into the current pipeline. So "build before probe" is literally the recursion
order.

## The crossing itself

1. **Producer side:** `gpu_pipeline_task::publish_output()` calls
   `sink->sink(output_data, stream)` (`gpu_pipeline_task.cpp:348`). The base
   `sink()` pushes every output batch to every `next_port_after_sink`:
   `next_operator->push_data_batch(port_name, batch)` →
   `port->repo->add_data_batch(batch)` (`sirius_physical_operator.cpp:238-261`).
   The batch now sits **idle in a shared_data_repository** — spillable.
2. **Consumer side:** the consumer's source operator pops one batch per port in
   `get_next_task_input_data()` → `repo->pop_next_data_batch()` (`:316-330`).

## Where the FULL barrier is CHECKED (the answer to the Stage-0 question)

**In the consumer's `get_next_task_hint()` (`sirius_physical_operator.cpp:275-314`),
not in the sink.** Two conditions:

1. Any FULL port whose `src_pipeline` is not finished →
   `WAITING_FOR_INPUT_DATA` (`p->type == FULL && !src_pipeline->is_pipeline_finished()`).
2. READY requires, for every port: non-FULL with data, **or** FULL with data
   **and** `src_pipeline->is_pipeline_finished()`.

So the barrier gates **task creation**, never a worker thread. A port with a
null repo is dependency-only (ordering without data).

## Completion predicate (`sirius_pipeline.cpp:361-406`)

Under `_status_mutex` (same lock as `get_task_creation_lock()`):

```
limit_exhausted
  || (first_node->is_source_pipeline_finished() && first_node->all_ports_empty())
  && tasks_created == tasks_completed
```

`mark_task_created()` fires in the task **constructor**; `mark_task_completed()`
in the **destructor**, which then calls `update_pipeline_status()`. Downstream
notification runs **outside** the lock (avoid holding a child mutex while taking
a parent's). This claim→construct→count-under-one-lock discipline is the race
fix QUEST's `Pipeline` must inherit (plan §2.5).

## The push loop (`gpu_pipeline_task.cpp:239-360`)

`compute_task()`: move input into a running buffer; `for (i = start_index;
i < operators.size(); i++)` → `run_one_operator()` = `op.execute(...)` +
`stream.synchronize()` per op; output feeds the next input. On OOM it throws
`oom_reschedule_exception` carrying the batch + current operator index `i` so
the retry **resumes mid-pipeline** (`start_index`). `publish_output()` then
hands the final batch to the sink.

## Consequences for QUEST's design

- The blocking op is in BOTH pipelines (source of current, sink of child) — plan
  §2.1 matches.
- QUEST's `RunTask` loop should keep a `start_index` parameter from day one
  (free now; needed for OOM-reschedule in the reservations phase).
- Barrier checks belong to the consumer-side claim path (Stage 4's
  runnable-set/claim logic), not to `PublishOutput`.
- Dependency-only edges (null repo) are a thing — MetaPipeline's FULL
  dependency between pipelines can exist without a BatchHolder when the
  blocking op carries state internally.
