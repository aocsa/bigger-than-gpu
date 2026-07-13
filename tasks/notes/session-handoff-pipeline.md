# Session Handoff — Pipeline Execution (resume at Stage 3)

> **SUPERSEDED.** Pipeline Stages 0–6 are complete. Use
> [`session-handoff-next.md`](session-handoff-next.md) for the next session
> (replication Phase 3 / Phase 1 bridge). Kept for historical Stage 3 context only.
>
> Written 2026-07-08 on branch `feature/refactor`.

## What this work is

Executing `docs/dev/pipeline-execution-plan.md`: converting QUEST's taskflow layer from
thread-per-operator into a push-oriented pipeline engine mimicking Sirius (pipelines split at
blocking operators, FULL/PIPELINE barrier semantics, forward push loop per batch).

**Process contract with the user:** one stage = one commit; BEFORE each commit present a
review summary of the changes; the user reviews (may request changes), then approves the
commit. Always keep `tasks/todo.md` updated while working.

## State at handoff

| Stage | Status | Commit |
|---|---|---|
| 0 — Sirius reading note (`tasks/notes/pipelines.md`) | ✅ done | `b789943` |
| 1 — Barrier vocabulary (`BarrierType`, `is_blocking_operator`, `IsBlocking()`) | ✅ done | `b789943` |
| 2 — Operator roles (`ProcessBatch` hoist, `IsSource`/`GetNextBatch`, `IsSink`/`Sink`/`FinalizeSink` + 3 test doubles) | ✅ done | `96d2673` |
| 3 — `Pipeline` + `MetaPipeline::Build` + shape tests | **designed, no code yet** — design below | — |
| 4 — `PipelineExecutor` | pending | — |
| 5 — Parity + benchmark | pending | — |
| 6 — Flip default | pending | — |

Test baseline: **498 passed / 0 failed / 1 skipped** (pre-existing `ParquetReaderTest.StringColumnRead`).
Uncommitted at handoff: only `tasks/todo.md` checkbox updates + this file (fold into the Stage 3 commit).

Build: `export PATH="/opt/homebrew/bin:$PATH"` first (cmake/ninja are Homebrew; zoxide prints a
harmless warning). `cmake --build build && ctest --test-dir build`.

## Key codebase facts (verified this session — don't re-derive)

- `Status` = `class Status : public std::expected<void, Error>` (`src/quest/result.hpp:231`).
  `Status::OK()`; `Status::Invalid("msg")` returns `std::unexpected<Error>` (converts to both
  `Status` and any `Result<T>`). `Result<T> = std::expected<T, Error>` (`result.hpp:432`).
  Propagate a `Result` error into a `Status`: `return std::unexpected(result.error());`.
- `cache::DataBlockPtr = std::shared_ptr<DataBlock>` (`data_block.hpp:204`). `DataBlock` has
  `num_rows()`, `size_in_bytes()`, `tier()`, `ToCpu()/ToDisk()`.
- `Graph::AddEdge` (`graph.cpp:71`) creates BatchHolders eagerly for EVERY edge and registers
  them on both ports. Default port names = `std::to_string(operator_id)` of the respective
  side; partitioned edges use `output_{i}`/`input_{i}`. Sentinel `-1` edges live only in
  `outgoing_edges_` (roots from sentinel, terminals to sentinel via `FinalizeWorkflow`);
  `incoming_edges_` holds only real edges.
- **`Graph` does NOT store `EdgeConfig`** — `AddEdge` consumes it. Stage 3 must add
  `std::map<std::pair<int32_t,int32_t>, EdgeConfig> edge_configs_` + `GetEdgeConfig(src,dst)`
  (side map; do NOT add fields to `Edge`, its `operator<=>` is used for set ordering).
- Test helpers: `quest::testing::MakeTestRecordBatch(rows)` in `src/quest/tests/utils.hpp`;
  wrap in `std::make_shared<HostMemoryBlock>(id, std::move(batch))` for a `DataBlockPtr`
  (pattern: `tests/test_batch_holder.cpp:28`).
- Stage 2 doubles (`VectorSourceOperator`, `AccumulatingSinkOperator`, `CountingStreamOperator`)
  are file-local in `tests/test_operator_roles.cpp` — NOT shared. For Stage 3 either define
  local mocks in the new test file or extract a shared `tests/` header (minor dedup, your call).
- `SimpleOperator` redeclares `ProcessBatch` pure (`override = 0`) — legacy contract preserved;
  `Operator::ProcessBatch` default is passthrough. `IsSink()` defaults to `IsBlocking()`;
  `IsSource()` defaults to `is_source_operator(type)`; `GetNextBatch()` EOS = engaged
  `std::nullopt` (NOT `Status::Invalid` — that hack lives only in the legacy queue path).
- Sirius ground truth is in `tasks/notes/pipelines.md` (file:line refs). Critical: the FULL
  barrier is checked in the consumer's `get_next_task_hint()` — barriers gate task creation,
  never a worker thread. `compute_task()` has a `start_index` for mid-pipeline OOM resume.

## Stage 3 design (worked out, ready to implement)

### Decision already made — holder creation does NOT move in Stage 3

The plan (§2.4/Stage 3 checklist) says "holder creation moves out of AddEdge into Build".
**Deviation, justified:** the legacy `Graph::Execute()` must keep working until Stage 6 and it
needs holders on every edge. So Stage 3 keeps `AddEdge` untouched; the pipelined path simply
never uses intra-pipeline holders. Holder-creation moves to Stage 6 when the legacy path
retires. Shape tests assert pipeline structure, not holder absence. Say this in the Stage 3
review summary and update the plan doc.

### New files

- `src/quest/taskflow/pipeline.{hpp,cpp}` — add `pipeline.cpp` to `QUEST_SOURCES` in root
  `CMakeLists.txt`.
- `src/quest/taskflow/meta_pipeline.{hpp,cpp}` — ditto.
- `tests/test_meta_pipeline.cpp` — register in `tests/CMakeLists.txt`.
- Header guards `QUEST_TASKFLOW_PIPELINE_HPP_` etc.; Doxygen `///`; copyright header.

### Pipeline class (sketch agreed with plan §2.2, refined)

```cpp
class Pipeline {
 public:
  explicit Pipeline(std::size_t pipeline_id);
  // structure (operators_[0] = source, back() = sink; blocking op appears in
  // BOTH its child pipeline (as sink) and parent pipeline (as source))
  void PrependOperator(Operator*);   // Build walks upstream
  [[nodiscard]] const std::vector<Operator*>& operators() const;
  [[nodiscard]] Operator* source() const;  // front()
  [[nodiscard]] Operator* sink() const;    // back()
  // dependencies
  void AddProducer(Pipeline* producer, BarrierType barrier);
  [[nodiscard]] const std::vector<Pipeline*>& FullBarrierProducers() const;
  [[nodiscard]] const std::vector<Pipeline*>& PipelineBarrierProducers() const;
  // accounting for Stage 4 (build now, per plan §2.5)
  void MarkTaskCreated(); void MarkTaskCompleted();
  [[nodiscard]] bool IsFinished() const; void MarkFinished();
  [[nodiscard]] std::mutex& status_lock();
  // the push loop; start_index=1 skips the source (it produced the batch);
  // pass 0 when the first operator is a holder-fed consumer. Keep start_index
  // from day one — future OOM-reschedule resumes mid-pipeline (Sirius does this).
  [[nodiscard]] Status RunTask(cache::DataBlockPtr batch, std::size_t start_index = 1);
 private:
  [[nodiscard]] Status PublishOutput(cache::DataBlockPtr batch);
  ...
};
using PipelinePtr = std::unique_ptr<Pipeline>;  // MetaPipeline::Build returns the owning vector;
                                                 // producer deps are raw Pipeline* into that vector
```

`RunTask`: loop `i in [start_index, size-1)` → `ProcessBatch`, propagate error via
`std::unexpected(result.error())`, short-circuit `Status::OK()` if batch null/0 rows.
`PublishOutput(batch)`: if `sink()->IsSink()` → `sink()->Sink(batch)`; else
`ProcessBatch` then `AddToOutput` (default cache_id resolves to the op-id port where AddEdge
registered the cross-pipeline holder — verified naming above). Single-op pipeline works:
loop is empty, PublishOutput handles operators_[0].

### MetaPipeline::Build algorithm (final)

```
Build(graph):
  for each terminal op (real outgoing edges empty): BuildFrom(terminal)
  memo: map<op_id, Pipeline*> for every built sink (handles shared producers/diamonds)
  children are pushed into the output vector BEFORE parents (recursion order) — this is
  the "build side before probe side" ordering guarantee the tests assert.

BuildFrom(sink_op):                       # builds the pipeline whose SINK is sink_op
  if memo[sink_op] exists → return it
  P = new Pipeline; P.Prepend(sink_op); cur = sink_op
  loop:
    incoming = graph.GetIncomingEdges(cur)          # real edges only
    if empty: break                                  # cur is the source
    if size == 1:
      up = node(edge.source); cfg = graph.GetEdgeConfig(edge)
      if up->IsBlocking():                           # Sirius: op in both pipelines
        P.Prepend(up); child = BuildFrom(up); P.AddProducer(child, Full); break
      if cfg.barrier == Full or cfg.num_partitions > 1:   # explicit break, op NOT shared
        child = BuildFrom(up); P.AddProducer(child, cfg.barrier); break
      P.Prepend(up); cur = up; continue              # fuse streaming op
    else:  # multi-input (join-shaped): one child per input, deterministic order
      for edge in incoming sorted by target_port_name:
        child = BuildFrom(node(edge.source)); P.AddProducer(child, cfg(edge).barrier)
      break
  register P; memo[sink_op] = P; return P
```

Notes: multi-input ops end up ONLY in the parent pipeline (children end at the upstream
producers, delivering via the op's named input holders) — asymmetric with the single-input
blocking case (op in both); this matches the plan's "two-port mock sink" test and defers
Sirius's operator-specific join layout to the real-joins phase. A `num_partitions > 1` edge is
the natural PIPELINE-barrier cross-pipeline case (fusing is impossible across partitioned
holders) — use it for the "PIPELINE edge → 2 pipelines, no FULL dep" shape test.

### Graph additions (small)

- `edge_configs_` map + `[[nodiscard]] EdgeConfig GetEdgeConfig(int32_t, int32_t) const`
  (returns `EdgeConfig{}` when absent) — populate at the end of `AddEdge`.
- Optional: `[[nodiscard]] std::vector<Operator*> GetTerminalOperators() const` (ops whose
  real outgoing edges are empty) — or compute inside `MetaPipeline::Build`.

### Shape tests to write (`tests/test_meta_pipeline.cpp`, build-only, no executor)

1. Linear `scan >> filter >> project` → 1 pipeline, ordered [scan, filter, project].
2. `scan >> filter >> agg >> project` (agg = `ComputeAggregate`) → 2 pipelines; agg is
   `back()` of child AND `front()` of parent; parent depends FULL on child; child precedes
   parent in the vector.
3. Two sources into a two-port mock sink (`a >> join["input_a"]`, `b >> join["input_b"]`,
   `join >> out`, join type `PartwiseJoin`) → 3 pipelines, both children before parent,
   parent has 2 producers, port-name-deterministic order (input_a's child first).
4. Explicit Full without blocking op: `link(filter, out, EdgeConfig::Simple().with_barrier(Full))`
   → 2 pipelines, filter NOT in the second one, FULL dep recorded.
5. Partitioned edge (`EdgeConfig::Partitioned(2)`) → 2 pipelines, PIPELINE-barrier dep,
   no FULL dep.
6. `RunTask` push loop: hand-assembled pipeline [VectorSource-like mock, 2× counting mock,
   accumulating IsSink mock] → both counters hit, sink received the batch; error mid-loop
   propagates; single-op pipeline publishes via AddToOutput.
7. Memo/diamond: one blocking producer feeding two consumers → child pipeline built once.

### Watch-outs

- `Edge` iteration order in `std::set<Edge>` is (target, source) — for multi-input, sort by
  `target_port_name` explicitly, don't rely on set order.
- Movable: `Pipeline` holds a `std::mutex` → non-copyable/non-movable; heap-allocate
  (`unique_ptr`), never store by value in vectors.
- clang-tidy in this repo flags short identifiers and magic numbers in src (tests are lenient);
  existing pre-existing warnings in kpair.hpp/operator.hpp are NOT ours to fix (minimal-impact rule).
- `*graph += a >> b` requires `Graph` created via `std::make_shared` (`shared_from_this` in
  `AddNode`). Test fixtures must use `make_shared<Graph>()`.

## Stage 4+ pointers (from the plan, unchanged)

Executor: worker pool (default `hw_concurrency`), runnable = all FULL producers finished,
claim-input → construct task → `MarkTaskCreated()` under `status_lock()` (the load-bearing
ordering from `tasks/notes/pipelines.md`), completion events re-evaluate the runnable set,
teardown order on error: stop creating → drain queue → join workers.
`Graph::ExecutePipelined()` added alongside untouched `Execute()` until Stage 6.

## The bigger picture

This is phase 2 of `docs/dev/sirius-replication-plan.md` (8-phase learn-by-doing roadmap;
interactive architecture page at `docs/quest-internals.html`). After pipelines: task-creator
hint chain (phase 3), memory reservations (4), predicate-driven spilling (5), real blocking
operators Limit→Aggregate→Sort→Join (6), vectorized expressions (7), honest streaming (8).
