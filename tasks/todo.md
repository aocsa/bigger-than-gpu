# Pipeline Execution — Working Plan

Executing [docs/dev/pipeline-execution-plan.md](../docs/dev/pipeline-execution-plan.md).
**Process:** one stage = one commit; each stage gets a review summary BEFORE committing; user reviews and may request changes.

## Stage 0 — Sirius reading note ✅ (commit b789943)
- [x] `tasks/notes/pipelines.md`: how one batch crosses a pipeline break in Sirius (folded into Stage 1 commit)

## Stage 1 — Barrier vocabulary ✅ (commit b789943)
- [x] `BarrierType {Pipeline, Full}` on `EdgeConfig` (`kpair.hpp`); `Concatenating()` implies `Full`
- [x] `is_blocking_operator(OperatorType)` in `operator_type.hpp`
- [x] `Operator::IsBlocking()` virtual consulting the type classification
- [x] Tests: `tests/test_barrier_config.cpp` (14 tests) registered in `tests/CMakeLists.txt`
- [x] Build green: 487 passed / 0 failed / 1 skipped (pre-existing)
- [x] REVIEW GATE: user approved Stage 0; Stage 1 landed in the same commit, summary delivered post-hoc

## Stage 2 — Operator roles ⏸ awaiting review
- [x] Hoist `ProcessBatch` to `Operator` (default passthrough); `SimpleOperator` redeclares it pure — legacy `run()` path untouched
- [x] `IsSource` (default: type classification) / `GetNextBatch` (EOS = engaged nullopt, not an error)
- [x] `IsSink` (default: `IsBlocking()`) / `Sink` (default: error) / `FinalizeSink` (default: OK no-op)
- [x] Test doubles: `VectorSourceOperator`, `AccumulatingSinkOperator` (mock blocking op with sink→source flip), `CountingStreamOperator`
- [x] `tests/test_operator_roles.cpp` (11 tests) incl. manual push-loop through all three roles
- [x] Build green: 498 passed / 0 failed / 1 skipped
- [ ] REVIEW GATE: summary delivered, awaiting user approval → commit

## Stage 3 — Pipeline + MetaPipeline::Build
- [ ] `pipeline.{hpp,cpp}`: operator list, counters, status lock, completion predicate, `RunTask`
- [ ] `meta_pipeline.{hpp,cpp}`: upstream walk, holders only on cross-pipeline edges
- [ ] Shape tests: linear->1; agg-in-middle->2 + FULL dep; two-port sink->build-before-probe; PIPELINE edge->holder present
- [ ] REVIEW GATE

## Stage 4 — PipelineExecutor
- [ ] Worker pool, runnable set, completion events, claim-under-lock, teardown order
- [ ] `Graph::ExecutePipelined()`; old `Execute()` untouched
- [ ] Tests: FULL gating, PIPELINE overlap, completion race stress, error propagation, 2-worker bound
- [ ] REVIEW GATE

## Stage 5 — Parity + benchmark
- [ ] Parity old == new; spill-through-holder test; 10M-row benchmark recorded
- [ ] /verify-operator + /review-mem
- [ ] REVIEW GATE

## Stage 6 — Flip default
- [ ] `Graph::Execute()` delegates to pipelined path; docs updated
- [ ] REVIEW GATE

## Review log

### 2026-07-08 — Stage 0+1 (`b789943`)
- 7 files, +378/−209. Pure vocabulary, no execution path reads `barrier` yet.
- Notable decision: `EdgeConfig::Concatenating()` now implies `BarrierType::Full`.
- Key Sirius finding captured in the note: the FULL barrier is checked in the
  consumer's `get_next_task_hint()` — barriers gate task creation, never a
  worker thread. `RunTask` will carry a `start_index` for future OOM resume.
- Process note: Stage 1 code was committed together with Stage 0 (tree was
  fully staged); review summary delivered after the commit instead of before.
