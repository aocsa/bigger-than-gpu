# Session Handoff — Pipeline Plan Complete; Resume at Replication Phase 3

> **For the next Claude Code / Cursor session.** Read this + [`tasks/todo.md`](../todo.md) +
> [`docs/dev/pipeline-execution-plan.md`](../../docs/dev/pipeline-execution-plan.md) +
> [`docs/dev/sirius-replication-plan.md`](../../docs/dev/sirius-replication-plan.md) before writing code.
> Written 2026-07-09 on branch `feature/refactor`.

## What this work is

Executing `docs/dev/pipeline-execution-plan.md`: converting QUEST's taskflow layer from
thread-per-operator into a push-oriented pipeline engine modeled on Sirius (pipelines split at
blocking operators, FULL/PIPELINE barrier semantics, forward push loop per batch).

That plan is **complete** (Stages 0–6 landed). It was Phase 2 of the broader
[`sirius-replication-plan.md`](../../docs/dev/sirius-replication-plan.md) (8-phase learn-by-doing
roadmap). The next unit of work is **replication Phase 3 — data-driven tasks + hint chain**.

**Process contract with the user:** one stage = one commit; BEFORE each commit present a
review summary of the changes; the user reviews (may request changes), then approves the
commit. Always keep `tasks/todo.md` updated while working. Independent review passes from a
second session have been common — expect hardening follow-up commits after the main stage
commit.

## State at handoff

| Stage | Status | Commit |
|---|---|---|
| 0 — Sirius reading note (`tasks/notes/pipelines.md`) | ✅ done | `b789943` |
| 1 — Barrier vocabulary | ✅ done | `b789943` |
| 2 — Operator roles | ✅ done | `96d2673` |
| 3 — `Pipeline` + `MetaPipeline::Build` | ✅ done | `f9108cd` + harden `8bcc39e` |
| 4 — `PipelineExecutor` | ✅ done | `d0cc379` + harden `61791b5` |
| 5 — Parity + spill + benchmark | ✅ done | `9d9a107` (with Stage 6) |
| 6 — Flip default + docs | ✅ done | `9d9a107` |

**Pipeline-execution-plan.md is DONE.** Flagship proof recorded: fused pipeline ~5× faster than
thread-per-operator on a 10M-row / 10k-batch passthrough plan (21 ms → 4 ms, Debug, Apple M).

Test baseline: **524 passed / 0 failed / 1 skipped** (`ParquetReaderTest.StringColumnRead`).
Branch: `feature/refactor`, ahead of origin by several commits (do not push unless asked).
Uncommitted at handoff: only `.claude/settings.local.json` (leave it alone — never commit it).

Build: `export PATH="/opt/homebrew/bin:$PATH"` first (cmake/ninja are Homebrew; zoxide prints a
harmless warning). `cmake --build build && ctest --test-dir build --output-on-failure`.
Discovery timeouts under load are flaky (5s `gtest_discover_tests`); retry the build — they are
not compile failures.

## What landed (architecture you inherit)

```text
Graph DSL (*graph += a >> b)          unchanged
        │
MetaPipeline::Build(graph)            splits at blocking ops / Full / partitioned edges
        │
vector<PipelinePtr>                   children before parents; Full deps recorded
        │
PipelineExecutor::Execute(...)        worker pool; claim → RunTask → FinishOnExit
        │
Graph::Execute()  ──delegates──►  ExecutePipelined()   ← DEFAULT
Graph::ExecuteLegacy()                DEPRECATED escape hatch for run()-only operators
```

Key files (all under `src/quest/taskflow/` unless noted):

| File | Role |
|---|---|
| `kpair.hpp` | `BarrierType {Pipeline, Full}`; `EdgeConfig::Concatenating()` implies Full |
| `operator_type.hpp` | `is_blocking_operator` / `is_source_operator` |
| `operator.hpp/.cpp` | Roles: `ProcessBatch`, `IsSource`/`GetNextBatch`, `IsSink`/`Sink`/`FinalizeSink` |
| `pipeline.hpp/.cpp` | Operator list, `RunTask(batch, start_index)`, `PublishOutput`, atomics + `status_lock_` |
| `meta_pipeline.hpp/.cpp` | `Build` / `BuildFrom`; cycle detection; rejects blocking→multi-input |
| `pipeline_executor.hpp/.cpp` | Worker pool, runnable set, claim, stall detection, teardown |
| `graph.hpp/.cpp` | `edge_configs_` + `GetEdgeConfig`; `GetTerminalOperators`; `Execute`/`ExecuteLegacy`/`ExecutePipelined` |
| `docs/dev/ARCHITECTURE.md` | Execution Model section rewritten for pipelines |
| `tasks/lessons.md` | Lessons #1 (benchmark axis) and #2 (flip audits contracts) |

Tests worth knowing:

- `tests/test_barrier_config.cpp`, `test_operator_roles.cpp`, `test_meta_pipeline.cpp`
- `tests/test_pipeline_executor.cpp` (concurrency + deadlock regressions)
- `tests/test_execution_parity.cpp` (parity, spill, 10M-row benchmark)

## Critical invariants (do not break)

1. **Claim → construct → `MarkTaskCreated()` under `pipeline.status_lock()`.** Completion
   increments (`MarkTaskCompleted`) may happen without the lock (atomics). See
   `tasks/notes/pipelines.md` and `pipeline.hpp` docs.
2. **`RunTask` start_index:** `source->IsSource() ? 1 : 0`. Holder-fed sources must run
   `ProcessBatch` on `operators()[0]` (Stage 4 P1 fix in `61791b5`).
3. **`FinishOnExit` RAII** in `DrainPipeline`: always `sink->output().Finish()` +
   `MarkFinished()` on every exit path — otherwise Pipeline-barrier consumers hang in `Pull()`.
4. **Stall detection:** `ready_queue_` empty + `in_flight_ == 0` + `outstanding_ > 0` →
   `Status::Invalid` (Full-barrier cycle among hand-assembled pipelines). Do not remove.
5. **`Pull()` Invalid ≠ always EOS.** Treat as EOS only when
   `holder->IsFinished() && !holder->HasNext()`; otherwise propagate (Stage 4 P3).
6. **ORDERING INVARIANT on `PipelineExecutor::Execute`:** Pipeline-barrier consumers can
   occupy workers while blocked in `Pull()`. Hand-assembled pipeline sets must keep producers
   schedulable (MetaPipeline's children-before-parents ordering masks this today). Real fix =
   non-blocking claim + requeue — Phase 3 hint chain.
7. **`status_lock()` held across unbounded `Pull()`** is documented deferred debt — fine while
   one drain per pipeline; must narrow before intra-pipeline parallelism.
8. **`PublishOutput` / `ClaimNextBatch` default-port only.** Partitioned edges
   (`output_{i}` / `input_{i}`) are shape-tested but not executed end-to-end yet.
9. **`ExecuteLegacy()`** still required by `tests/test_graph.cpp` mocks and
   `examples/{simple_graph,join_example}.cpp` (run()-only). Delete only after those migrate to
   pipeline roles.
10. **Holders still created in `AddEdge` for every edge** (Stage 3 deviation). Pipelined path
    ignores intra-pipeline holders. Moving holder creation to Build was deferred past Stage 6
    because `ExecuteLegacy` still needs them — still open cleanup once Legacy dies.

## Known open gaps (carry into next work)

| Gap | Where | When to fix |
|---|---|---|
| Partitioned-edge publish/claim | `PublishOutput`, `ClaimNextBatch` | When an executor first runs partitioned pipelines |
| Blocking op → multi-input consumer | `MetaPipeline::WalkMultiInputProducers` rejects | Real joins / relay pipelines (replication Phase 6) |
| Worker starvation with Pipeline-barrier consumers | Executor ORDERING INVARIANT | Phase 3 hint chain (non-blocking claim) |
| `status_lock` across `Pull()` | `DrainPipeline` | Intra-pipeline task parallelism |
| `ExecuteLegacy` + run()-only mocks | `test_graph.cpp`, examples | Before deleting Legacy |
| Holders on every edge | `Graph::AddEdge` | After Legacy deletion |
| `/verify-operator` + `/review-mem` not run on Stage 5/6 | noted in todo review log | Optional; Stage 5/6 was mostly tests + 10-line flip |

## What to do next

### Immediate housekeeping (optional, small)

1. Tick Stage 4/5/6 REVIEW GATE boxes in `tasks/todo.md` (commits already landed; checkboxes
   still show `[ ] REVIEW GATE` in places — cosmetic).
2. Mark Phase 2 as landed in `docs/dev/sirius-replication-plan.md` scorecard:
   `2 — Pipelines | ☑ | fused pipeline beats thread-per-operator (~5× recorded)`.
3. Optionally migrate `test_graph` / examples to pipeline roles and delete `ExecuteLegacy()`.

### Next real work — Replication Phase 3 (hint chain)

From `sirius-replication-plan.md` Phase 3:

- `TaskCreationHint {Ready, WaitingForInput}` + `GetNextTaskHint()` / `GetNextTaskInputData()`
  on `Operator`.
- `Task = {Pipeline*, DataBlockPtr}`; `TaskCreator` recursion; `TaskScheduler` over the existing
  worker pool.
- Replace "one worker drains one pipeline sequentially" with batch-granular tasks.
- This is also the natural place to fix the Pipeline-barrier worker-starvation ORDERING
  INVARIANT (non-blocking claim + requeue-as-waiting).

**Working agreement still applies:** read Sirius first (`task_creator.cpp`,
`task_scheduler.cpp`), write ≤1 page in `tasks/notes/`, plan in `tasks/todo.md`, then implement.

Phase 1 of the replication plan (bridge the two engines / real taskflow operators for Scan /
Filter / Project) is still ☐ — pipelines currently run on **mocks**. Real end-to-end spill from
a QueryBuilder query still needs Phase 1. Decide with the user whether Phase 3 (scheduler) or
Phase 1 (bridge) comes next; the pipeline chassis is ready for either.

## Key codebase facts (still true — don't re-derive)

- `Status` = `std::expected<void, Error>`; `Result<T> = std::expected<T, Error>`; propagate with
  `return std::unexpected(result.error());` (`src/quest/result.hpp`).
- `DataBlockPtr = shared_ptr<DataBlock>`; wrap test batches with
  `make_shared<HostMemoryBlock>(id, MakeTestRecordBatch(rows))`.
- Default port names = `to_string(operator_id)`; partitioned = `output_{i}` / `input_{i}`.
- `Edge` stays identity-only; configs live in `Graph::edge_configs_` + `GetEdgeConfig`.
- `GetTerminalOperators()` ignores sentinel `target_operator_id < 0` (safe after
  `FinalizeWorkflow()`).
- EOS on the new path = engaged `std::nullopt` from `GetNextBatch()` — never `Status::Invalid`
  as EOS (except the corroborated holder-finished case above).
- Sirius ground truth: `tasks/notes/pipelines.md`. FULL barrier gates **task creation**, never
  a parked worker.

## Lessons already logged (`tasks/lessons.md`)

1. Benchmark the axis the engines differ on (batch count, not just row count).
2. Flipping a default means auditing every caller's **contract**, not just call sites
   (`ExecuteLegacy` exists because of this).

## Bigger picture

```text
sirius-push-based-event-drive.md     WHY (classification)
sirius-replication-plan.md           WHAT (8 phases) — Phase 2 DONE via pipeline-execution-plan
pipeline-execution-plan.md           HOW for Phase 2 — Stages 0–6 DONE
tasks/todo.md                        tracker (may need REVIEW GATE checkbox cleanup)
this file                            resume here
```

Interactive architecture page: `docs/quest-internals.html`.
