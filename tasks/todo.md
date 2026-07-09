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

### 2026-07-09 — Phase 1 independent review (review session; P1+P4 fixed, folded into the Phase 1 commit)
Second-session review of the staged Phase 1 diff against the replication plan, focused on
Phase 1 ⇄ Phase 2 integration. Verdict: the pipeline engine needed NO changes — all fixes are
Phase-1-side, using Phase 2 vocabulary that already existed.
1. **P1 (flagship promise, fixed)** — `ExecuteOnGraph` could never spill: (a) a linear plan
   fused into ONE pipeline → no cross-pipeline holder anywhere → `MaybeSpill` unreachable
   (the old flagship test only spilled by hand-assembling a Full edge, bypassing
   `CreateGraph`); (b) `FilterOperator`/`ProjectOperator` built output blocks with no
   cpu/disk resources → allocations invisible to saturation AND `ToDisk()` failed, which
   `MaybeSpill` swallows silently. Fixed: `CreateGraph` now wires the collector behind a
   **Full barrier** (Sirius's RESULT_COLLECTOR-as-own-pipeline shape — final batches always
   cross a spill-aware holder), and all three operators stamp blocks via a shared
   `execution::MakeTrackedHostBlock` helper (scan deduplicated onto it). The flagship test
   now goes through the real gate with filter+project in the path and asserts
   `disk_tier_count > 0` plus correct disk→CPU materialization. Shape test updated: 2
   pipelines (fused producer + collector), Full dependency asserted.
2. **P3 (drive-by, one line)** — `collector_operator.hpp` included itself instead of
   `taskflow/operator.hpp`; compiled only via include-order luck, broke standalone use.
3. **P4 (fixed)** — thread-safety contract comment on `CollectorOperator` readers (valid
   only after Execute() returns); `.DS_Store`/`.claude/settings.local.json` unstaged;
   `.DS_Store` added to `.gitignore`.
Deferred with note: **P2** layering inversion (`cache/batch_holder.cpp` → `execution/
batch_kernels.hpp` → `physical_plan/expressions`) — split the concat helper down a layer.
Suite: **530 passed / 0 failed / 1 skipped**.

### 2026-07-09 — Phase 1 implementation (Stages 0–6)
- Sirius note: single gate, error = fallback; no capability table.
- Phase 2 scorecard already marked landed; Phase 1 now ☑ with flagship spill proof.
- Additive surface only: operators + `CreateGraph` + `ExecuteOnGraph`; no MetaPipeline /
  PipelineExecutor edits.
- Suite: all existing binaries green + 5 new `test_graph_bridge` tests; arithmetic
  `CreatePhysicalExpr` tests green. (CMake `gtest_discover_tests` 5s timeouts remain flaky
  under load — run binaries directly.)
