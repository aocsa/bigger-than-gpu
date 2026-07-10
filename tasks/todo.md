# GraphBuilder DSL Replacement — Working Plan

Executing [docs/dev/graph-builder-dsl.md](../docs/dev/graph-builder-dsl.md) as a
**replacement** of the old taskflow wiring DSL (`>>` / `link()` / `operator+=` /
`kpair.hpp`), not an additive surface. Logical-plan fluent API
(`QueryBuilder::FromParquet.Where.Select`) stays.

**Process:** one stage = one review gate; keep this file updated while working.

**Decisions locked (design §9):** `Chain()` refines its LAST edge; `Prepare()` returns a
`PreparedQuery` (states as types); `AddTuningPolicy` chain; `ScaleFlowControl` ships as a
documented stopgap. **Ordering:** land before replication phase 4 — `Prepare(stats, hw)`
is where reservation estimates plug in later.

## Stage D0 — Rebaseline design for replacement ✅
- [x] Design doc goals/non-goals: `GraphBuilder` is the single supported wiring API
- [x] Old DSL (`>>` / `link()` / `operator+=`) marked as migration targets to delete
- [x] §8 reframed as migration gallery (not coexistence)
- [x] `sirius-replication-plan.md` Phase 2.5 updated to D0–D5 replacement stages
- [x] This todo rewritten to the replacement plan
- [x] REVIEW GATE

## Stage D1 — Deferred holders + PreparedQuery (behavior-preserving) ✅
- [x] `QueryStats` / `HardwareInfo` / `PreparedQuery` types
- [x] Graph stores edge SPECS; holders materialize in `Prepare()` (retires Stage-6 debt)
- [x] `PreparedQuery` owns resolved configs, holders, pipelines, executor
- [x] `Graph::Execute()` = `Prepare({},{}).Execute()`
- [x] Empty `SynthesizeStructure()` hook before resolution
- [x] Temporary `Materialize()` public seam for hand-assembled tests
- [x] Existing suite green + focused Prepare independence tests (`test_prepared_query`)
- [x] REVIEW GATE

## Stage D2 — GraphBuilder + port contracts + validation ✅
- [x] Chain (last-edge refinement), Edge overloads, Named, Config, ThenTo, Apply(Shape)
- [x] `DeclaredInputPorts()` / `InputEdgeContract()`; Collector contract = Full
- [x] Build(): port-declaration validation, acyclicity, contract-conflict, duplicate names, Partial() rejected
- [x] `test_graph_builder` covering §8 migration gallery + validation cases (22 tests)
- [x] REVIEW GATE

## Stage D3 — Migrate all call sites onto GraphBuilder ✅
- [x] `CreateGraph` uses `quest::taskflow::GraphBuilder` (collector Full via contract)
- [x] Migrate tests: walkthrough, graph, meta_pipeline, pipeline_executor, barrier_config, graph_bridge, parity, prepared_query
- [x] Migrate examples (`simple_graph.cpp`, `join_example.cpp`)
- [x] Remaining old-DSL refs only in historical docs / kpair until D4
- [x] REVIEW GATE

## Stage D4 — Delete the old DSL surface ✅
- [x] Remove `kpair.hpp`, `>>`, `link()`, `Graph::operator+=`
- [x] Remove KPair tests; barrier_config uses `edge_config.hpp`
- [x] Clean includes / CMake; search gate for old patterns outside docs
- [x] Full graph-related suite green
- [x] REVIEW GATE

## Stage D5 — Runtime tuning + soft knobs ✅
- [x] `EdgeRef` / names / `Edges()` introspection; synthesis naming stub
- [x] `AddTuningPolicy` (ordered), resolution precedence chain (§5)
- [x] `Graph::Tune()`; `PreparedQuery::SetFlowControl/SetSpillPolicy/ScaleFlowControl`
- [x] Holder thresholds → atomics + `NotifySaturationWaiters()` re-wake
- [x] `SetMaxInFlightTasks` reserved (returns Invalid until phase 3)
- [x] Flagship tests: precedence, policy ordering, live re-wake, re-Prepare independence
- [x] REVIEW GATE

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

### 2026-07-09 — Stage D5 runtime tuning + soft knobs
Landed Phase 2.5 D5 against [graph-builder-dsl.md](../docs/dev/graph-builder-dsl.md) §5–§7.
- `EdgeRef`/`EdgeInfo`, `Graph::Edges()` (Built), `PreparedQuery::Edges()` (resolved);
  `Named()` propagated onto `Graph::EdgeSpec`.
- `AddTuningPolicy` ordered chain + `Graph::Tune` / `EdgeTuner`; Prepare order is
  synthesize → resolve (non-mutating Built specs) → materialize → MetaPipeline.
- Live soft knobs on `PreparedQuery`; holder FC thresholds / auto_spill are atomics
  with `NotifySaturationWaiters()` (+ phase-3 re-eval comment).
- `SetMaxInFlightTasks` returns Invalid ("reserved until phase 3").
- Scorecard Phase 2.5 row marked ☑ in `sirius-replication-plan.md`.
- Tests: `test_runtime_tuning` **7/7 PASS** (PolicyOrdering, RePrepareIndependence,
  LiveSetFlowControlReleasesBlockedProducer, ScaleFlowControlHalvesAllEdges,
  SetMaxInFlightTasksReturnsInvalid, NamedEdgeTuneAndEdgesIntrospection,
  SpillPolicyNeverMapsToAutoSpillFalse). Regression: prepared_query, graph_builder,
  graph, batch_holder, barrier_config, meta_pipeline, pipeline_executor, graph_bridge,
  walkthrough, execution_parity — all green.

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
  under load — run binaries directly).
