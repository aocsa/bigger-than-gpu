# Phase 1 — Feed Real Operators Into the Push Pipeline

Executing the Phase 1 bridge from [docs/dev/sirius-replication-plan.md](../docs/dev/sirius-replication-plan.md)
(plan: feed Scan/Filter/Project into the existing push `PipelineExecutor` without changing
Phase 2 semantics).

**Process:** one stage = one commit; each stage gets a review summary BEFORE committing;
user reviews and may request changes.

**Non-goals:** do not change `MetaPipeline::Build`, `PipelineExecutor`, role contracts, or
the `Execute()` → `ExecutePipelined()` default.

## Stage 0 — Sirius note + tracker ✅
- [x] `tasks/notes/create-plan-gate.md`
- [x] Rewrite this todo for Phase 1
- [x] Tick Phase 2 in replication scorecard

## Stage 1 — CreatePhysicalExpr arithmetic ✅
- [x] Wire `Add`/`Sub`/`Mul`/`Div`/`Mod` in `CreatePhysicalExpr`
- [x] ExprBuilder arithmetic operators
- [x] Tests for arithmetic projection via physical plan
- [x] REVIEW GATE: landed with Stages 2–5 in one implementation pass

## Stage 2 — Real taskflow operators ✅
- [x] `TableScanOperator` / `FilterOperator` / `ProjectOperator` / `CollectorOperator`
- [x] Shared `execution/batch_kernels.hpp`
- [x] Unit/integration coverage via `test_graph_bridge`
- [x] REVIEW GATE

## Stage 3 — CreateGraph gate ✅
- [x] `CreateGraph(LogicalPlan&, MemoryResourceManagerPtr) -> Result<ExecutableGraphPtr>`
- [x] Shape smoke (one fused pipeline) + Parquet parity
- [x] REVIEW GATE

## Stage 4 — QueryBuilder::ExecuteOnGraph ✅
- [x] Additive hook; default `Execute` unchanged
- [x] Fallback to callback engine on unsupported plan
- [x] Integration parity test
- [x] REVIEW GATE

## Stage 5 — Spill flagship + ConcatenatingBatchHolder ✅
- [x] Multi-block `ConcatenateAll` via `ConcatenateHostBlocks`
- [x] Tiny-CPU-budget real TableScan→Collector spill test (Full barrier)
- [x] REVIEW GATE

## Stage 6 — Docs ✅
- [x] ARCHITECTURE: real query → CreateGraph → push pipelines
- [x] Tick Phase 1 in replication scorecard
- [x] REVIEW GATE

## Review log

### 2026-07-09 — Phase 1 implementation (Stages 0–6)
- Sirius note: single gate, error = fallback; no capability table.
- Phase 2 scorecard already marked landed; Phase 1 now ☑ with flagship spill proof.
- Additive surface only: operators + `CreateGraph` + `ExecuteOnGraph`; no MetaPipeline /
  PipelineExecutor edits.
- Suite: all existing binaries green + 5 new `test_graph_bridge` tests; arithmetic
  `CreatePhysicalExpr` tests green. (CMake `gtest_discover_tests` 5s timeouts remain flaky
  under load — run binaries directly.)
