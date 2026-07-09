# GraphBuilder DSL + Runtime Edge Tuning — Working Plan

Executing [docs/dev/graph-builder-dsl.md](../docs/dev/graph-builder-dsl.md) (design
finalized 2026-07-09: fluent builder, `PreparedQuery` lifecycle, policy-chain tuning,
phase-3/5/6 readiness per its §10 matrix).

**Process:** one stage = one commit; each stage gets a review summary BEFORE committing;
user reviews and may request changes. Keep this file updated while working.

**Decisions locked (design §9):** `Chain()` refines its LAST edge; `Prepare()` returns a
`PreparedQuery` (states as types); `AddTuningPolicy` chain; `ScaleFlowControl` ships as a
documented stopgap. **Ordering:** land before replication phase 4 — `Prepare(stats, hw)`
is where reservation estimates plug in later.

## Stage D0 — Design ✅ (docs/dev/graph-builder-dsl.md)
- [x] Fluent builder API (Chain/Edge/EdgeBuilder/Apply/Build) fully documented
- [x] Lifecycle: Graph (Built, structural tuning) → PreparedQuery (soft tuning)
- [x] Runtime tuning: EdgeRef/names, introspection, policy chain, live soft knobs
- [x] Phase 3/5/6 readiness pass: wake contract, Spill(SpillPolicy), Partial() reserved,
      PartitionedUpTo (late-bound structural), synthesis-before-resolution + synthesized-edge names
- [ ] REVIEW GATE: user reviews the design doc → then implementation stages below

## Stage D1 — Deferred holders + PreparedQuery (the load-bearing refactor)
- [ ] Graph stores edge SPECS; holders materialize in `Prepare()` (retires Stage-6 debt)
- [ ] `PreparedQuery` owns resolved configs, holders, pipelines, executor; `Graph::Execute()` = `Prepare({},{}).Execute()`
- [ ] `ExecuteLegacy()` keeps eager semantics for its remaining callers
- [ ] All 532 existing tests green (behavior-preserving)
- [ ] REVIEW GATE

## Stage D2 — GraphBuilder + EdgeBuilder/ChainBuilder + validation
- [ ] Chain (last-edge refinement rule), Edge overloads, Named, Config, ThenTo, Apply(Shape)
- [ ] Build(): port-declaration validation, acyclicity, contract-conflict, duplicate names, Partial() rejected
- [ ] Examples from design §8 as tests (old-DSL parity where applicable)
- [ ] REVIEW GATE

## Stage D3 — Port contracts
- [ ] `DeclaredInputPorts()` / `InputEdgeContract()` virtuals; resolution layer 4
- [ ] `CollectorOperator` contract = Full; `CreateGraph` loses its special-case link
- [ ] Typed-port accessor pattern documented on one operator
- [ ] REVIEW GATE

## Stage D4 — Tuning: policy chain + live soft knobs
- [ ] `AddTuningPolicy` (ordered), resolution precedence chain (§5) incl. synthesis hook stub
- [ ] `Graph::Tune()` (structural, Built only by construction), `PreparedQuery::SetFlowControl/SetSpillPolicy/ScaleFlowControl`
- [ ] Holder thresholds → atomics + `NotifySaturationWaiters()` re-wake; `SetMaxInFlightTasks` reserved (returns Invalid until phase 3)
- [ ] Tests: precedence, policy ordering, live re-wake under a blocked producer, re-Prepare independence
- [ ] REVIEW GATE

---

# ✅ COMPLETED — Phase 1: Feed Real Operators Into the Push Pipeline

Executed the Phase 1 bridge from [docs/dev/sirius-replication-plan.md](../docs/dev/sirius-replication-plan.md)
(feed Scan/Filter/Project into the existing push `PipelineExecutor` without changing
Phase 2 semantics). Commits `d6685aa` + hardening `a65e447`.

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
