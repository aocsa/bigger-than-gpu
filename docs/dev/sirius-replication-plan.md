# Sirius Replication Plan — Learn by Doing

**Purpose:** understand how Sirius (NVIDIA's DuckDB-based GPU query engine) works internally, then replicate its core architectural ideas in QUEST — this CPU/Disk engine — at a small scale, one concept at a time. The goal is *learning*, not feature parity: each phase teaches one Sirius idea by making you build its CPU-scale equivalent with your own hands.

**Companion:** interactive architecture page at [`docs/quest-internals.html`](../quest-internals.html) — the component map and comparison there mirror the phases here. GPU-engine lineage + QUEST study destination (pipelines shipped / scheduling next): [`docs/sirius-vs-blazingsql.html#quest-study`](../sirius-vs-blazingsql.html#quest-study).

**Sirius source:** `/Users/aocsa/git/aocsa/sirius` (branch `docs-tutorial`). Layout note: implementation in `src/<subsys>/*.cpp`, headers mirrored under `src/include/<subsys>/*.hpp`.

---

**BlazingSQL source:** [`docs/kt-bigger-than-gpu.html`](../kt-bigger-than-gpu.html) — the
knowledge-transfer page for the ancestor engine (`/Users/aocsa/git/blazingsql`). QUEST's
`OperatorType` enum and `EdgeConfig` descend directly from BlazingSQL's `kernel_type` and
`cache_settings`; the full abstraction map lives in
[graph-builder-dsl.md §11](graph-builder-dsl.md).

---

## Implementation order

**1 ✅ → 2 ✅ → 2.5 ✅ (DSL) → 2.6 ✅ (BlazingSQL edge parity) → 2.7 ✅ (single-node operator catalog) → 3 → 4 → 5 → 6 → 7 → 8.**

Phase 2.5 (GraphBuilder DSL + `PreparedQuery` lifecycle + runtime tuning,
[graph-builder-dsl.md](graph-builder-dsl.md)) was inserted after phases 1+2 landed. It goes
**before phase 3** because: (a) its `PreparedQuery` is where phase 3's task creator will
live — landing it first makes phase 3 a rewrite-in-place instead of rewrite-then-move;
(b) its deferred-holder refactor retires the eager-`AddEdge` debt phase 3 would otherwise
work around; (c) it ships the simple CV-based live-tuning wake that phase 3 then swaps for
the task-re-arm event per the documented wake contract. The rest of the order is unchanged —
the dependency chain still holds: reservations gate *tasks* (4 after 3), downgrade *serves*
reservations (5 after 4), out-of-core joins need downgrade for their flagship (6 after 5),
7 and 8 are orthogonal tails. Phases 4/5/6 each consume something 2.5 provides
(`Prepare(stats, hw)`; `SpillPolicy`; port contracts + `PartitionedUpTo` + the synthesis hook).

## Where QUEST stands (status snapshot, 2026-07-09, post-2.7)

Phases 1, 2, 2.5, 2.6, and 2.7 are landed (606 tests green, 1 skipped; through commit `5179bcf`):

- **The two-engines gap is closed.** `QueryBuilder::ExecuteOnGraph()` → `CreateGraph` gate
  (error = fallback to the callback engine) → `MetaPipeline::Build` → `PipelineExecutor`.
  The pipelined engine is the default (`Graph::Execute()` delegates; legacy survives as
  deprecated `ExecuteLegacy()` until its `run()`-based callers migrate).
- **Out-of-core is real:** a Parquet query with filter+project under a 256-byte budget
  spills through the collector's Full-barrier holder and returns correct results
  (`test_graph_bridge` flagship). Fused pipelines measured ~5× faster than
  thread-per-operator on per-batch overhead.
- **The wiring layer is fully modernized:** `GraphBuilder` → `Graph` (Built) →
  `Prepare(stats, hw)` → `PreparedQuery`, with the policy-chain tuning, live soft knobs,
  and all four BlazingSQL cache flavors (SIMPLE / CONCATENATING+byte-cap / NON_WAITING /
  FOR_EACH lanes) — phases 2.5 + 2.6.
- **9 of 19 operators wired end-to-end** (Scan, Filter, Projection, Output/Collector,
  plus phase 2.7's Limit, Union, BindableTableScan, Print, Generate). The heavy
  decomposed kernels (aggregate/sort/join, `*SingleNode` partition/merge) are phase 6;
  the `Distribute*`/shuffle kernels stay reserved (multi-node, out of scope).
- **Known-bug ledger** (original baseline list, annotated):
  - ~~`CreatePhysicalExpr` arithmetic~~ — **fixed in phase 1** (`d6685aa`).
  - ~~`ConcatenatingBatchHolder` multi-block~~ — **fixed in phase 1** (`ConcatenateHostBlocks`).
  - `StreamingQueryBuilder` fake streaming — open, owned by **phase 8**.
  - Arithmetic double-upcast / left-operand promotion / nulls compare `false` /
    materialized columns drop nulls — open, owned by **phase 7**.
  - Advisory memory accounting (`cols × size × 8`), macOS RAM hardcoded 8 GB — open,
    owned by **phase 4** ("fix accounting first").

## The Sirius ideas worth stealing (ranked by learning value)

| # | Sirius idea | One-line essence | QUEST phase |
|---|---|---|---|
| 1 | Single plan gate with fallback | `create_plan()` translates or throws; the CPU plan is the executor of last resort | 1 |
| 2 | Pipelines from blocking operators | streaming ops fuse; blocking ops end one pipeline and source the next | 2 |
| 2b | Runtime-decided configs | Sirius binds partition counts, thresholds, strategies at runtime from real sizes (`determine_num_partitions`, settings) — not at plan time | 2.5 |
| 2c | Cache flavors + gate-time kernel decomposition (BlazingSQL) | edges pick a cache machine flavor via `cache_settings` (SIMPLE/CONCATENATING/NON_WAITING/FOR_EACH); `PhysicalPlanGenerator` emits the kernel pipeline (sample→partition→merge) BEFORE any pipeline concept | 2.6 + 6 |
| 3 | Data-driven task creation (hint chain) | nothing polls; operators answer READY/WAITING and the creator recurses to the deepest ready producer | 3 |
| 4 | Reserve memory before dispatch | a task secures its estimated peak (history-corrected, clamped) or it doesn't run | 4 |
| 5 | Predicate-driven spilling + batch lock states | spill *idle* batches until "does the reservation fit now?" passes; idle ⇄ locked makes it safe | 5 |
| 6 | Partition/merge decomposition of blocking ops | GROUP BY = partition → per-partition agg → merge; ORDER BY = sample sort; JOIN = partition both sides | 6 |
| 7 | One expression AST, pluggable strategies | translate frontend exprs once at plan time; evaluate columnar; strategy is a setting | 7 |
| 8 | Bounded channels + honest streaming | byte-capped admission, close-then-drain EOS, handles-not-owners | 8 |

---

## Phase 0 — Read Sirius first (1–2 sessions, no code)

Before building anything, ground the mental model in real code. Read in this order (verified paths, sized for one sitting each):

1. `src/include/exec/channel.hpp` (134 lines) — the cleanest file in the repo: `publisher<T>::send()`, `channel<T>::get()/try_get()/close()`, built on `interruptible_mpmc`. Compare with QUEST's `cache/blocking_queue.hpp` — they solve the same problem.
2. `src/include/memory/sirius_memory_reservation_manager.hpp` (47 lines) — the entire reservation surface. Tiny, load-bearing.
3. `src/include/expression/ast/node.hpp` + `src/expression/ast/node.cpp` (96 lines) — the 12-alternative `std::variant` AST; note the "alternative order is ABI, append-only" rule (QUEST's QRBT serializer has the same footgun).
4. `src/transparent/sirius_optimizer_extension.cpp` (101 lines) — how Sirius intercepts DuckDB: pre-hook disables 3 optimizers, post-hook restores + stores `LogicalOperator::Copy()`.
5. `src/include/op/sirius_physical_operator.hpp` (552 lines) — the central abstraction. Read for the four roles: `execute()` (batch-in/batch-out), memory estimate (`no_history_peak_memory_estimate`, default 2× input), source (`get_next_task_hint`, `get_next_task_input_data`), sink (`sink()`, `is_sink()`). Note the nested `port` struct with `MemoryBarrierType {PIPELINE, PARTIAL, FULL}`.

**Exercise:** write a one-page summary in your own words: "how does a batch get from a parquet file to a result in Sirius?" If you can't, re-read `src/creator/task_creator.cpp` (422 lines) — `get_operator_for_next_task()` is the recursion at the heart of everything.

---

## Phase 1 ✅ — Bridge the two engines (the `create_plan()` gate)

**Sirius idea:** one function (`sirius_physical_plan_generator::create_plan()`, `src/planner/sirius_physical_plan_generator.cpp`, 324 lines + one file per node type) translates the whole plan or throws; on throw, the untouched CPU plan runs. Support *is* whatever the gate can translate — no capability table.

**Build in QUEST:**
- `src/quest/execution/graph_builder.hpp` — `CreateGraph(const LogicalPlan&, MemoryResourceManagerPtr) -> Result<GraphPtr>`.
- Three **real** taskflow operators in `src/quest/taskflow/operators/`: `TableScanOperator`, `FilterOperator`, `ProjectOperator` — each wraps the existing physical-exec logic (`ScanExec`'s DataSource iteration, `SelectionExec`'s mask+gather, `ProjectionExec`'s expr eval) inside `SimpleOperator::ProcessBatch()`. Batches travel as `HostMemoryBlock`-wrapped `RecordBatch`es through `BatchHolder`s.
- A `CollectorOperator` sink that gathers final batches (needs `ConcatenatingBatchHolder` fixed — do it here: column-wise `RecordBatch` append).
- `QueryBuilder::ExecuteOnGraph()` (or a `use_taskflow` flag): try `CreateGraph()`; on unsupported node, **fall back to the callback engine** — QUEST's own executor of last resort. Same throw→fallback discipline as Sirius, at CPU scale.

**Also fix here:** the `CreatePhysicalExpr` arithmetic bug (add the `Add/Sub/Mul/Div/Mod` branch) — you'll be reusing physical exprs inside operators and will hit it immediately.

**Done when:**
- The same query returns identical results via both engines (add an integration test asserting this).
- A query over a Parquet file larger than an artificially tiny CPU budget **spills to `/tmp/quest_cache` and still returns correct results** — the first real out-of-core query in the project's history. Make this the flagship test.

**Rules compliance:** new operators implement `run()` AND `can_you_throttle_my_input()`; every new file gets a registered test.

---

## Phase 2 ✅ — Pipelines (blocking ops are the seam)

**Sirius idea:** `sirius_meta_pipeline::build()` walks the operator tree; streaming ops (FILTER, PROJECTION, LIMIT) append to the current pipeline; blocking ops (JOIN, ORDER_BY, aggregates) become sinks and spawn child meta-pipelines (build side before probe side). A blocking operator is the sink of one pipeline and the source of the next. Read: `src/pipeline/sirius_meta_pipeline.cpp`, then skim the seven-phase `sirius_pipeline_converter.cpp` (1353 lines — the largest file; don't replicate it, just understand *why* each phase exists).

**Build in QUEST:**
- `Pipeline` = ordered list of operators, fused: one thread executes scan→filter→project per batch instead of one thread per operator with a queue between each. (This alone is a measurable perf win — fewer handoffs.)
- `MetaPipeline::Build(Graph) -> vector<Pipeline>` that splits at blocking operators.
- Edge barrier semantics on `EdgeConfig`: `Pipeline` (streaming, today's behavior) and `Full` (consumer can't start until producer pipeline finished). You don't need `Partial` until phase 6's joins.

**Done when:** a scan→filter→project query runs as ONE pipeline (one thread, no intermediate queues), and a synthetic two-pipeline plan (using a mock blocking op) honors the `Full` barrier. Benchmark fused vs per-operator-thread on a 10M-row file; record the numbers in the test.

---

## Phase 2.5 — GraphBuilder DSL + PreparedQuery + runtime tuning (inserted)

**Full design:** [graph-builder-dsl.md](graph-builder-dsl.md) (decisions locked in its §9;
phase-3/5/6 readiness matrix in its §10). **Working plan:** `tasks/todo.md` stages D0–D5.
**Replacement, not additive:** after this phase the old taskflow wiring DSL (`>>` /
`link()` / `Graph::operator+=` / `kpair.hpp`) is deleted. The logical-plan fluent API
(`QueryBuilder::FromParquet.Where.Select`) stays.

**Sirius idea:** configs are bound when sizes are known, not when the plan is written —
Sirius picks partition counts at runtime (`determine_num_partitions` from real input
bytes) and barrier/port semantics are decided by operator code, never by the query
author. QUEST's equivalent: defer holder materialization to `Prepare(stats, hw)`, let
operators declare port contracts, and let policies tune edges when Parquet footer
statistics and hardware info exist.

**Build in QUEST (stages D0–D5):**
- **D0** — rebaseline design docs: builder is the single supported wiring API; old DSL
  is a migration target, not a coexistence surface.
- **D1** — deferred holders + `PreparedQuery`: `Graph` stores edge *specs*; holders
  materialize in `Prepare()`; `PreparedQuery` owns resolved configs, holders, pipelines,
  executor. Behavior-preserving (retires the Stage-6 eager-`AddEdge` debt).
- **D2** — `GraphBuilder`/`EdgeBuilder`/`ChainBuilder` (last-edge refinement rule),
  port contracts, `Apply(Shape)`, `Build()` validation (declared ports, acyclicity,
  contract conflicts, duplicate names, `.Partial()` rejected until phase 6).
  Collector declares Full; `CreateGraph` loses its special-case link.
- **D3** — migrate all production + test call sites (`CreateGraph`, walkthrough,
  graph/meta-pipeline/executor/barrier tests, examples) onto `GraphBuilder`.
- **D4** — delete `kpair.hpp`, `>>`, `link()`, and `Graph::operator+=`.
- **D5** — tuning: `AddTuningPolicy` chain resolved at `Prepare()`, `Graph::Tune()`
  (structural, Built-only by type), `PreparedQuery::SetFlowControl/SetSpillPolicy/
  ScaleFlowControl` (soft, live, atomic + re-wake).

**Done when:** the same `Graph` `Prepare()`d twice with different stats yields different
holder configs; a live `SetFlowControl` raise releases a producer blocked in
`WaitIfSaturated()`; the §8 migration gallery runs as builder-only tests; no
`kpair.hpp` / `link(` / graph `>>` call sites remain outside historical docs.

---

## Phase 2.6 — BlazingSQL edge/kernel parity, CPU-only (inserted)

**Reference:** [`docs/kt-bigger-than-gpu.html`](../kt-bigger-than-gpu.html) ("The cache
system model", "Cache machines — the types, and the class map", the kernel catalog).
**Abstraction map:** [graph-builder-dsl.md §11](graph-builder-dsl.md) — BlazingSQL
`kernel`/`port`/`CacheMachine`/`cache_settings` ⇄ QUEST `Operator`/`Port`/`BatchHolder`/
`EdgeConfig`, adapted for CPU-only (L1 = CPU RAM, L2 = disk; the consumer always gets a
CPU `RecordBatch` back — `decache()` ≡ `ToCpu()`).

**BlazingSQL idea:** every edge picks a cache-machine *flavor* via `cache_settings`
(`link(partition, merge, cache_settings{.type = FOR_EACH, .num_partitions = N})`), and
heavy operators are decomposed into kernel pipelines by the `PhysicalPlanGenerator` at
plan-gate time — the physical operator list exists **before** pipelines are derived.

**Build in QUEST:**
- **`kNonWaiting` holder type** (BlazingSQL `NonWaitingCacheMachine`): non-blocking pull
  for pre-filled, single-shot handoffs (the direct-execution pattern); rejected as a
  Pipeline-barrier streaming edge (its contract is "data already there").
- **Chunked concatenation** (mature `concat_cache_num_bytes` / Sirius
  `_concat_batch_bytes`): `EdgeConfig::concat_bytes_threshold` +
  `EdgeBuilder::ConcatenatingUpTo(bytes)` — the concatenating holder drains-and-merges
  up to N bytes per delivered batch instead of all-or-nothing. Stats-tunable via the
  policy chain (this cap should never be author-guessed).
- **EdgeBuilder vocabulary**: `NonWaiting()`, `ConcatenatingUpTo(bytes)` (design §11.4);
  port contracts and `MetaPipeline`/executor handling for both.
- **Gate-time decomposition seam**: `CreateGraph` grows the `PhysicalPlanGenerator` role —
  one logical node may emit N operators + FOR_EACH edges (structure visible to the
  policy chain as author-written edges). Phase 6 fills in the real decompositions; this
  phase establishes the seam and proves it with a mock two-stage decomposition.

**Done when:** a pre-filled `kNonWaiting` edge feeds a pipeline without blocking; a
`ConcatenatingUpTo` edge delivers multiple merged chunks whose sizes respect the byte cap
(and a policy can resize the cap from `QueryStats`); a mock decomposed node (one logical
op → two operators + FOR_EACH edge) flows through `CreateGraph → MetaPipeline → executor`
with correct lane naming (`output_0…N-1`/`input_0…N-1`).

---

## Phase 2.7 — The single-node operator catalog (inserted)

**The authoritative list of operators/kernels this project implements.** Derived from
QUEST's `OperatorType` enum (which IS BlazingSQL's v0.14 kernel list) crossed with the
KT doc's kernel catalog, scoped to **single-node execution only**. Every distributed/
shuffle kernel stays a reserved enum value.

**Why before phase 3** (from [sirius-vs-blazingsql.html §quest-study](../sirius-vs-blazingsql.html#quest-study)):
the task scheduler needs a wider operator surface to stress — multi-input ports (Union),
early pipeline termination (Limit), and gate rewrites (BindableTableScan) all exist
*before* the executor is rewritten, so phase 3 lands against real shapes instead of mocks.

**Read first:** the KT doc's simple-kernel catalog
([kt-bigger-than-gpu.html §kernels](../kt-bigger-than-gpu.html)) and Sirius's streaming
LIMIT claim pattern (`src/op/sirius_physical_limit.cpp` — an atomic claim on remaining
rows, no compute).

| Kernel (BlazingSQL) | QUEST `OperatorType` | What it does | Status / owner |
|---|---|---|---|
| `table_scan` | `TableScan` | drives the DataSource, pushes batches as they load | ✅ `TableScanOperator` (phase 1) |
| `filter` | `Filter` | predicate → mask → compacted batch | ✅ `FilterOperator` (phase 1) |
| `project` | `Project` | evaluates expressions per batch | ✅ `ProjectOperator` (phase 1) |
| output kernel | `Output` | terminal sink; the query result | ✅ `CollectorOperator` (phase 1) |
| `limit` | `Limit` | truncate the stream to n rows (atomic claim — no compute) | ✅ **phase 2.7** — `LimitOperator`; retired the hardcoded-1000 `OptimizeScanWithLimit`; stops the upstream scan early |
| `union` | `Union` | concatenate two streams (UNION ALL); multi-input `input_a`/`input_b` | ✅ **phase 2.7** — `UnionOperator`; executor drains declared named ports in order |
| `bindable_tablescan` | `BindableTableScan` | scan with column pruning + folded filters — reads less | ✅ **phase 2.7** — gate folds `Projection(Scan)` of pure column refs (predicate pushdown completes in phase 8) |
| debug print sink | `Print` | row-dump sink for debugging pipelines | ✅ **phase 2.7** — `PrintOperator` (CSV to injectable ostream) |
| row generator | `Generate` | synthetic source (dev/test utility, real source role) | ✅ **phase 2.7** — `GenerateOperator` (configurable synthetic source) |
| `ComputeAggregateKernel` | `ComputeAggregate` | local groupby partials (sink role) | ☐ phase 6 (groupby: Compute → Merge) |
| `MergeAggregateKernel` | `MergeAggregate` | re-aggregates partials (COUNT partials merge by SUM) | ☐ phase 6 |
| `SortAndSample` | `SortAndSample` | per-batch local sort + sample pivots; two output ports (`output_a` data, `output_b` plan) | ☐ phase 6 (orderby, 3-stage) |
| `PartitionSingleNode` | `PartitionSingleNode` | range-split at pivots → FOR_EACH lanes | ☐ phase 6 — **lane infra shipped in 2.6 (E3)** |
| `MergeStream` | `MergeStream` | k-way merge, one lane per partition | ☐ phase 6 |
| `PartwiseJoin` | `PartwiseJoin` | joins cached batch pairs `input_a × input_b` incrementally | ☐ phase 6 (join capstone; + `Partition` for out-of-core lanes) |
| `DistributeAggregateKernel` | `DistributeAggregate` | multinode shuffle stage | ✋ **reserved** — out of single-node scope |
| `JoinPartitionKernel` | `JoinPartition` | multinode hash-shuffle for joins | ✋ **reserved** |
| `LogicalPartition` (multinode) | `Partition` | all-to-all exchange split | ✋ **reserved** (single-node uses `PartitionSingleNode`) |

**Build in QUEST (phase 2.7 proper — the five simple kernels):** each ships the full
recipe — logical node (where applicable) + taskflow operator on the pipeline roles +
port contract + typed ports + tests via `/new-node` & `/new-op`:
1. **Limit** — streaming, atomic remaining-rows claim; early pipeline termination
   (the executor's limit hooks exist on `Pipeline`); wire `Limit()` into `QueryPlan`.
2. **Union** — multi-input streaming (`input_a`/`input_b` contracts); pulls both
   streams, forwards in arrival order; first consumer of the multi-input claim path.
3. **BindableTableScan** — fold `Projection` directly into the scan at the gate
   (projection pruning); the `CreateGraph` gate learns its first rewrite.
4. **Print** — pass-through debug sink with row formatting (uses `RecordBatch::ToCSV`).
5. **Generate** — configurable synthetic source; replaces ad-hoc test sources.

**Done when:** `FromParquet(f).Where(...).Select(...).Limit(n)` runs end-to-end through
the gate with a real `LimitOperator` (no hardcoded 1000); a UNION ALL of two scans
returns both inputs' rows through the graph engine; the gate emits `BindableTableScan`
when a `Projection` sits directly on a `Scan` and the parquet reader provably reads
fewer columns.

---

## Phase 3 — Data-driven tasks + scheduler (the hint chain)

**Sirius idea:** work exists only when data does. Operators answer `get_next_task_hint() -> READY{producer} | WAITING_FOR_INPUT_DATA{producer}`; on WAITING the task creator **recurses into the producer** until something is READY; a task = (pipeline, one input batch); a scheduler matches tasks to ready workers. Read: `src/creator/task_creator.cpp` (`get_operator_for_next_task`, `manager_loop`) and `src/pipeline/task_scheduler.cpp` (374 lines).

> **Notation (2026-07-09) — BlazingSQL has no analogue; do not port from RAL.**
> After reviewing [`docs/blazingsql-internals.html`](../blazingsql-internals.html) and
> [`docs/kt-bigger-than-gpu.html`](../kt-bigger-than-gpu.html): v0.14.0
> `graph.execute()` is **thread-per-kernel** with blocking `pullFromCache` only —
> there is no task unit, no hint chain, no worker-pool scheduler, and no
> reserve-before-dispatch. Phase 3 is therefore **Sirius-guided** (see
> [`docs/sirius.html`](../sirius.html) tasks section), not a BlazingSQL heritage
> port. Contrast with phases 2 / 2.6, where the lineage *is* BlazingSQL → Sirius →
> QUEST (flat kernel graph → meta-pipelines → `MetaPipeline`; CacheMachine flavors →
> `EdgeConfig`). Teaching summary: [`docs/sirius-vs-blazingsql.html#quest-study`](../sirius-vs-blazingsql.html#quest-study).

**Build in QUEST:**
- `TaskCreationHint {Ready, WaitingForInput}` + `GetNextTaskHint()` / `GetNextTaskInputData()` virtuals on `Operator`.
- `Task = {Pipeline*, DataBlockPtr input}`; `TaskCreator` implementing the recursion; a `TaskScheduler` over a fixed worker pool (`std::jthread` × N, one shared queue — skip per-device executors, you have one "device").
- Replace `PipelineExecutor`'s **one-worker-drains-one-pipeline loop** (phase 2 already killed thread-per-operator) with task-granular scheduling, **inside `PreparedQuery`** (phase 2.5 put the executor there — this phase is a rewrite-in-place). Mind Sirius's ordering invariant: **mark task created before popping input**, or a pipeline can look finished mid-pop.
- This retires the documented executor ordering-invariant hazard (a Pipeline-barrier consumer parked in `Pull()` occupying a worker): a consumer without input becomes a *non-created task*, never a blocked thread.
- Honor the **wake contract** from the DSL design (§6.5): `SetFlowControl` must post a task-re-evaluation event to the creator (replacing the CV-notify that phase 2.5 shipped), and un-reserve `PreparedQuery::SetMaxInFlightTasks()`.

**Done when:** queries run correctly on a 4-worker pool regardless of plan width; a `Full`-barrier consumer never receives a task before its producer pipeline finishes; throughput scales with workers on a multi-pipeline plan; a live threshold raise on a WAITING producer mints its task without any unrelated completion. Stress-test with the pattern from `verify-operator`.

---

## Phase 4 — Memory reservations (admission control)

**Sirius idea:** reserve before dispatch. The executor clamps the ask to `get_max_memory()` (kills a livelock where an over-capacity estimate spins forever), `make_reservation()` must succeed before the task runs, estimates start at 2× input bytes and are corrected by a 64-entry ring of (estimated, actual peak) per pipeline; OOM ratchets estimates up. Read: `src/include/memory/sirius_memory_reservation_manager.hpp` first, then `src/pipeline/gpu_pipeline_executor.cpp` (478 lines) for reserve→dispatch.

**Build in QUEST:**
- Fix accounting first: `HostMemoryBlock::size_in_bytes()` must return real bytes (sum of column buffers + null bitmaps + string heaps), and macOS should use `sysctl(HW_MEMSIZE)` (`memory/memory_resource.cpp:28`).
- `Reservation` RAII type on `MemoryResourceManager`: `MakeReservation(bytes) -> Result<Reservation>`; release on destruction.
- `Operator::EstimatePeakMemory(input_bytes)` (default 2×) + a `PipelineMemoryHistory` ring that corrects future estimates with observed peak/estimate ratios.
- Scheduler integration: clamp → reserve → run; failed reservation requeues the task (and, after phase 5, triggers a downgrade request instead of spinning).
- Plug into phase 2.5's seam: reservation sizing happens at/under `Prepare(stats, hw)` —
  `QueryStats`/`HardwareInfo` are already the shared vocabulary, and the macOS
  `sysctl(HW_MEMSIZE)` fix doubles as `HardwareInfo` probing.
- **Control-plane warning** ([sirius-vs-blazingsql.html §gap](../sirius-vs-blazingsql.html#gap)):
  BlazingSQL's ancestor mechanism — counter-based demotion at `addToCache` ("spill because
  the cache is full") — is a useful CONTRAST but the wrong template. This phase builds the
  Sirius control plane: "refuse to run until memory is reserved, then spill until the
  reservation fits." QUEST's current `MaybeSpill` IS the BlazingSQL pattern; phases 4+5
  replace it, not extend it.

**Done when:** with a 100 MB budget, a query whose working set is 500 MB either succeeds via spill or fails with a clean `ResourceExhausted` — never OOM-kills the process. A test asserts estimates converge (ring average within 2× of observed after 10 batches).

---

## Phase 5 — Predicate-driven spilling + batch lock states

**Sirius idea:** spilling serves reservations. `request_downgrade(predicate)` where the predicate is "does `make_reservation_or_null(bytes)` succeed now?" — a dedicated `downgrade_executor` spills *idle* batches until the predicate passes, then stops; the three-state lock (idle ⇄ read_only ⇄ mutable) on `data_batch` is what makes "idle = spillable" safe; the return trip is lazy (next task upgrades on use). Read: `src/downgrade/downgrade_executor.cpp` (526 lines) — processing thread, monitor thread, worker pool.

**Build in QUEST:**
- Three-state lock on `DataBlock`: `Idle ⇄ ReadOnly ⇄ Mutable`, with RAII accessors (`ReadLock`, `WriteLock`) — data reachable *only* through a lock, so an idle block provably has no readers.
- `DowngradeExecutor`: one background thread + `RequestDowngrade(predicate) -> std::future<size_t>`; candidates = idle blocks enumerated from `BatchHolder`s (back-to-front, like Sirius partitions), **honoring phase 2.5's per-edge `SpillPolicy`** (`Never` = skip, `First` = victim-order front — the join build side's hint); convert via existing `ToDisk()`, rollback on failure.
- Rewire: `BatchHolder::Add` stops spilling inline (`MaybeSpill` goes away); a failed reservation in the scheduler issues a downgrade request and retries when the future resolves. Add trigger/stop hysteresis (e.g. trigger 0.9, stop 0.7) so it doesn't thrash.

**Done when:** under memory pressure the executor spills the *oldest idle* blocks only, stops as soon as the pending reservation fits, never touches a locked block (assert via a test that holds a ReadLock during pressure), and a spilled block transparently upgrades on next use.

---

## Phase 6 — The blocking operators: Limit → Aggregate → Sort → Join

**Sirius idea:** every blocking operator decomposes into partition/merge stages so each task stays bounded: `HASH_GROUP_BY → PARTITION → MERGE_GROUP_BY`; ORDER_BY = three-pipeline sample sort (local sort → sample/partition → k-way merge, one partition per task); hash join = PARTITION→CONCAT on *both* sides, left = probe (streamed), right = build (materialized), build behind a FULL barrier. Read per-operator: `src/op/sirius_physical_grouped_aggregate.cpp` + `_merge`, `src/op/sirius_physical_sort_sample.cpp` / `_sort_partition` / `_merge_sort`, `src/op/sirius_physical_hash_join.cpp`, and the planner side `src/planner/sirius_plan_aggregate.cpp` / `_comparison_join.cpp`.

**Build in QUEST, in order (each = logical node + physical node + taskflow operator + tests, via the `/new-node` and `/new-op` skills). Every operator ships phase 2.5's extension recipe — port contracts (`DeclaredInputPorts`/`InputEdgeContract`), typed port accessors, and a `Shape` helper — so wiring is correct by construction. Partition counts arrive as `PartitionedUpTo(max)` bounds, bound by the operator at runtime from real input sizes; `.Partial()` unblocks here.**

**Decomposition placement (BlazingSQL model, per phase 2.6):** the multi-operator
topologies below are emitted by `CreateGraph` at **plan-gate time** — the QUEST
`PhysicalPlanGenerator` — with FOR_EACH lanes between partition and merge stages, exactly
as [`kt-bigger-than-gpu.html`](../kt-bigger-than-gpu.html)'s kernel catalog draws them.
QUEST builds the **single-node topologies** (CPU-only: no shuffle/`Distribute*` stages —
those enum values stay reserved): orderby = `SortAndSample → PartitionSingleNode(FOR_EACH)
→ MergeStream`; groupby = `ComputeAggregate → MergeAggregate`; join = `PartwiseJoin`
(+ `Partition` as the out-of-core mechanism — each lane fits the budget independently).
The sample stage keeps BlazingSQL's two-output-port shape (`output_a` data, `output_b`
partition plan). `Prepare()`'s `SynthesizeStructure()` hook stays for structure that
genuinely needs stats, but the gate is the default place structure is born.
Historical note: BlazingSQL's own gate decomposition
(`transform_operator_bigger_than_gpu`) was hard-coded OFF at v0.14 — QUEST's phase 6
effectively ships what the ancestor gated: the mature always-on decomposition, minus
the shuffle stages.

1. **Limit** — the warm-up. `StreamingLimit` with an atomic claim on remaining rows (Sirius does exactly this — no compute, just arithmetic). Replace the hardcoded 1000-row hack in `Graph::OptimizeScanWithLimit` (`graph.cpp:12`) with a real pushed limit. *Teaches:* early pipeline termination.
2. **Hash aggregate** — the first cross-batch state. `HashGroupBy` (sink accumulates per-batch partial aggregates into a hash table keyed by group columns) → `Merge` (combines partials). Support SUM/COUNT/MIN/MAX/AVG — and follow Sirius: **AVG = SUM + COUNT finalized at merge**, don't implement it as a primitive. Add `GroupBy()` to `QueryPlan`'s fluent API. *Teaches:* sink role, partial/final decomposition, why merges exist.
3. **Sort** — sample sort in three pipelines: per-batch local sort → sample to choose range boundaries → range-partition → per-partition k-way merge (one partition per task). Spill-friendly by construction: partitions are independent. *Teaches:* multi-pipeline choreography, why SORT_SAMPLE must see enough data before partitioning.
4. **Hash join** — the capstone. `PartitionOperator` (hash-partition both inputs by key, same partition count both sides — Sirius's "sibling link") → per-partition build+probe with the build side behind a `Full` barrier into a named `"build"` port (QUEST's `Port` already supports named slots — this is why it exists). Convention: **left = probe, right = build.** *Teaches:* everything above at once, plus out-of-core joins (each partition pair fits the budget independently).

**Done when:** a TPC-H-Q1-shaped query (`scan → filter → group-by → aggregate → sort`) and a two-table join query both run through the graph engine, produce results matching a pandas/duckdb oracle on the same data, and complete under a memory budget smaller than the data (forcing spill mid-join).

---

## Phase 7 — Vectorized expressions (one AST, pluggable strategies)

**Sirius idea:** translate frontend expressions once at plan time into an engine-neutral AST (`std::variant` of 12 alternatives); one evaluator with two verbs (`evaluate` for projection, `select` for filter); strategy is a setting — MATERIALIZE (one kernel per node) vs AST_INTERPRET (fuse the tree into one pass, default) vs AST_JIT. Nodes with no fused equivalent become "AST breakers": materialized and re-referenced. Read: `src/include/expression/ast/node.hpp`, `src/expression_executor/gpu_expression_executor.cpp` (383 lines), one specialization (`specializations/gpu_execute_comparison.cpp`).

**Build in QUEST:**
- Columnar evaluation: `Evaluate(RecordBatch) -> ColumnVector` operating on whole typed arrays (tight loops over `TypedColumnVector<T>` spans, null bitmaps processed word-at-a-time). Keep the row-at-a-time path as a second strategy behind an enum — QUEST's MATERIALIZE vs (a modest) fused path, selectable per query for A/B benchmarking.
- Fix the semantic debt in the same pass, with tests first: proper type promotion (`int32 + float → float`, don't upcast everything to `double`), int64-preserving arithmetic, SQL 3-valued logic for null comparisons, null tracking in materialized columns.
- Stretch: a `FilterProject` fused operator that evaluates the predicate and projection in one pass over each batch (the CPU analog of "one kernel for the whole tree").

**Done when:** the columnar path produces identical results to the row path across a property-style test (random exprs × random data), and benchmarks ≥10× faster on a 10M-row filter. Precision test: `SUM` of int64 values near 2⁶³ is exact.

---

## Phase 8 — Honest streaming + scan improvements

**Sirius idea:** bounded channels with byte-capped admission (reject a push that would cross the byte bound — unless the channel is empty, so one oversized batch can't wedge the stream), close-then-drain EOS (`closed() && drained()`), and queued items as *handles* that stay spillable. On the scan side: split coalescing to a byte target (512 MB), footer caching, stats-based row-group pruning, reader-side predicate pushdown. Read: `src/include/exec/channel.hpp` again (now you'll see it differently), `src/scan_manager/load_balancing_scan_batch_coalescer.cpp`, `src/scan_manager/split_provider.cpp`.

**Build in QUEST:**
- Evolve `BlockingQueue` → `Channel`: add `capacity_bytes` with the oversized-batch admission rule and overflow-safe byte math; add on-close callbacks; keep `Finish()`→`closed()` semantics. Replace the `Status::Invalid`-means-EOS hack with an explicit `Closed` state.
- Make `StreamingQueryBuilder` honest: `ExecuteStream()` runs the graph engine on a background scheduler and `co_yield`s batches pulled from the sink's channel — real backpressure end to end (a slow consumer throttles the scan).
- Scan: configurable target batch bytes (coalesce small row groups / split reads), row-group pruning from Parquet column statistics (min/max vs pushed predicates), and predicate pushdown from `Selection` into `Scan` as a `LogicalPlan` rewrite pass — your first optimizer rule.

**Done when:** a consumer that sleeps 100 ms/batch keeps peak memory flat (backpressure works); a selective query over a sorted column skips row groups (assert reader stats); batch size is stable at the target regardless of file layout.

---

## Working agreement (how to run each phase)

1. **Read the Sirius files first** (listed per phase), write a ≤1-page "how Sirius does it" note in `tasks/notes/` — *then* design the QUEST version. The note is the learning artifact; the code is the proof.
2. **Plan mode + `tasks/todo.md`** per phase (per CLAUDE.md workflow); one phase = one branch = one PR-sized change set.
3. **Every phase ends green:** all existing tests + new tests pass; new operators go through `/verify-operator`; memory-touching phases through `/review-mem`.
4. **Don't gold-plate:** Sirius details that exist for GPUs (streams, NUMA pinning, device colocation, kvikio) are explicitly out of scope. When in doubt, build the smallest thing that teaches the idea.
5. **Keep score:** append a row to the table below when a phase lands.

| Phase | Landed | Flagship proof |
|---|---|---|
| 1 — Bridge | ☑ | real Parquet scan→filter→project via CreateGraph; spill under tiny CPU budget (`test_graph_bridge`) |
| 2 — Pipelines | ☑ | fused pipeline ~5× faster than thread-per-operator (21 ms → 4 ms, 10k batches × 1k rows, Debug) |
| 2.5 — DSL + tuning | ☑ | same Graph `Prepare()`d twice with different stats → different holder configs; live threshold raise releases a blocked producer |
| 2.6 — BlazingSQL parity | ☑ | pre-filled NonWaiting edge executes single-shot; ConcatenatingUpTo chunks at a policy-tuned byte cap; Partitioned(2) FOR_EACH fan-out/drain runs end-to-end (`test_blazing_parity`, 8 tests) |
| 2.7 — Operator catalog | ☑ | Limit through the gate (no hardcoded 1000, early source stop); UNION ALL via named multi-port drain; BindableTableScan folds Projection→scan (provably fewer columns); Generate/Print utilities (`test_limit`/`test_union`/`test_bindable_scan`/`test_generate_print`, 40 tests) |
| 3 — Tasks | ☐ | 4-worker pool, barrier ordering test |
| 4 — Reservations | ☐ | 500 MB query under 100 MB budget: spill or clean error |
| 5 — Downgrade | ☐ | spills idle-only, stops when reservation fits |
| 6 — Operators | ☐ | TPC-H-Q1 shape + out-of-core join vs oracle |
| 7 — Vectorized | ☐ | ≥10× filter speedup, exact int64 SUM |
| 8 — Streaming | ☐ | flat memory under slow consumer; row-group pruning |
