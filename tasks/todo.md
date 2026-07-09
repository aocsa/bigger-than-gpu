# Pipeline Execution — Working Plan

Executing [docs/dev/pipeline-execution-plan.md](../docs/dev/pipeline-execution-plan.md).
**Process:** one stage = one commit; each stage gets a review summary BEFORE committing; user reviews and may request changes.

## Stage 0 — Sirius reading note
- [ ] `tasks/notes/pipelines.md`: how one batch crosses a pipeline break in Sirius (folded into Stage 1 commit)

## Stage 1 — Barrier vocabulary
- [ ] `BarrierType {Pipeline, Full}` on `EdgeConfig` (`kpair.hpp`); `Concatenating()` implies `Full`
- [ ] `is_blocking_operator(OperatorType)` in `operator_type.hpp`
- [ ] `Operator::IsBlocking()` virtual consulting the type classification
- [ ] Tests: `tests/test_barrier_config.cpp` registered in `tests/CMakeLists.txt`
- [ ] Build green, all existing tests + new tests pass
- [ ] REVIEW GATE: summary -> user approval -> commit

## Stage 2 — Operator roles
- [ ] Hoist `ProcessBatch` to `Operator` (default passthrough); keep `SimpleOperator::run()` legacy path
- [ ] `IsSource`/`GetNextBatch` (EOS = nullopt), `IsSink`/`Sink`/`FinalizeSink`
- [ ] Test doubles: `VectorSourceOperator`, `AccumulatingSinkOperator`, `CountingStreamOperator`
- [ ] REVIEW GATE

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
(filled as stages complete)
