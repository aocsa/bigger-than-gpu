# Sirius Replication Plan — Learn by Doing

**Purpose:** understand how Sirius (NVIDIA's DuckDB-based GPU query engine) works internally, then replicate its core architectural ideas in QUEST — this CPU/Disk engine — at a small scale, one concept at a time. The goal is *learning*, not feature parity: each phase teaches one Sirius idea by making you build its CPU-scale equivalent with your own hands.

**Companion:** interactive architecture page at [`docs/quest-internals.html`](../quest-internals.html) — the component map and comparison there mirror the phases here.

**Sirius source:** `/Users/aocsa/git/aocsa/sirius` (branch `docs-tutorial`). Layout note: implementation in `src/<subsys>/*.cpp`, headers mirrored under `src/include/<subsys>/*.hpp`.

---

## Where QUEST stands today (baseline)

Verified by full code review (2026-07-08, 473/473 tests green):

- **Two disconnected engines.** Engine A (`builder/` → `logical_plan/` → `physical_plan/`) runs every real query as a single-threaded recursion of `std::function` callbacks — it never touches memory tiers or spilling. Engine B (`taskflow/` + `cache/` + `memory/`) has the DAG, blocking queues, backpressure, and CPU→Disk spill — but its only `Operator` subclasses are mocks in `tests/test_graph.cpp`. **The project's namesake feature (out-of-core execution) is unreachable from a real query.**
- **3 of 19 operators wired end-to-end** (Scan, Selection/Filter, Projection). No Limit, Aggregate/GroupBy, Sort, Join, Union.
- **Known bugs to fix along the way:**
  - `CreatePhysicalExpr` doesn't translate arithmetic exprs — `Select(Col("a") + Col("b"))` fails at plan time (`physical_plan/physical_plan.hpp:439` area, binary branch handles only comparisons/And/Or).
  - `ConcatenatingBatchHolder::ConcatenateAll` errors on >1 block (`cache/batch_holder.cpp:213`).
  - `StreamingQueryBuilder::ExecuteStream` buffers everything before yielding (fake streaming).
  - All arithmetic upcasts to `double`; type promotion uses left operand; nulls compare `false` (no 3-valued logic); materialized columns drop null tracking.
  - Memory accounting is advisory (`cols × size × 8` estimate), macOS RAM detection hardcoded to 8 GB.

## The Sirius ideas worth stealing (ranked by learning value)

| # | Sirius idea | One-line essence | QUEST phase |
|---|---|---|---|
| 1 | Single plan gate with fallback | `create_plan()` translates or throws; the CPU plan is the executor of last resort | 1 |
| 2 | Pipelines from blocking operators | streaming ops fuse; blocking ops end one pipeline and source the next | 2 |
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

## Phase 1 — Bridge the two engines (the `create_plan()` gate)

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

## Phase 2 — Pipelines (blocking ops are the seam)

**Sirius idea:** `sirius_meta_pipeline::build()` walks the operator tree; streaming ops (FILTER, PROJECTION, LIMIT) append to the current pipeline; blocking ops (JOIN, ORDER_BY, aggregates) become sinks and spawn child meta-pipelines (build side before probe side). A blocking operator is the sink of one pipeline and the source of the next. Read: `src/pipeline/sirius_meta_pipeline.cpp`, then skim the seven-phase `sirius_pipeline_converter.cpp` (1353 lines — the largest file; don't replicate it, just understand *why* each phase exists).

**Build in QUEST:**
- `Pipeline` = ordered list of operators, fused: one thread executes scan→filter→project per batch instead of one thread per operator with a queue between each. (This alone is a measurable perf win — fewer handoffs.)
- `MetaPipeline::Build(Graph) -> vector<Pipeline>` that splits at blocking operators.
- Edge barrier semantics on `EdgeConfig`: `Pipeline` (streaming, today's behavior) and `Full` (consumer can't start until producer pipeline finished). You don't need `Partial` until phase 6's joins.

**Done when:** a scan→filter→project query runs as ONE pipeline (one thread, no intermediate queues), and a synthetic two-pipeline plan (using a mock blocking op) honors the `Full` barrier. Benchmark fused vs per-operator-thread on a 10M-row file; record the numbers in the test.

---

## Phase 3 — Data-driven tasks + scheduler (the hint chain)

**Sirius idea:** work exists only when data does. Operators answer `get_next_task_hint() -> READY{producer} | WAITING_FOR_INPUT_DATA{producer}`; on WAITING the task creator **recurses into the producer** until something is READY; a task = (pipeline, one input batch); a scheduler matches tasks to ready workers. Read: `src/creator/task_creator.cpp` (`get_operator_for_next_task`, `manager_loop`) and `src/pipeline/task_scheduler.cpp` (374 lines).

**Build in QUEST:**
- `TaskCreationHint {Ready, WaitingForInput}` + `GetNextTaskHint()` / `GetNextTaskInputData()` virtuals on `Operator`.
- `Task = {Pipeline*, DataBlockPtr input}`; `TaskCreator` implementing the recursion; a `TaskScheduler` over a fixed worker pool (`std::jthread` × N, one shared queue — skip per-device executors, you have one "device").
- Replace `Graph::Execute`'s thread-per-operator BFS with: schedule the scan; after each task completes, offer downstream consumers to the creator. Mind Sirius's ordering invariant: **mark task created before popping input**, or a pipeline can look finished mid-pop.

**Done when:** queries run correctly on a 4-worker pool regardless of plan width; a `Full`-barrier consumer never receives a task before its producer pipeline finishes; throughput scales with workers on a multi-pipeline plan. Stress-test with the pattern from `verify-operator` (the project has a skill for adversarial operator testing — use it).

---

## Phase 4 — Memory reservations (admission control)

**Sirius idea:** reserve before dispatch. The executor clamps the ask to `get_max_memory()` (kills a livelock where an over-capacity estimate spins forever), `make_reservation()` must succeed before the task runs, estimates start at 2× input bytes and are corrected by a 64-entry ring of (estimated, actual peak) per pipeline; OOM ratchets estimates up. Read: `src/include/memory/sirius_memory_reservation_manager.hpp` first, then `src/pipeline/gpu_pipeline_executor.cpp` (478 lines) for reserve→dispatch.

**Build in QUEST:**
- Fix accounting first: `HostMemoryBlock::size_in_bytes()` must return real bytes (sum of column buffers + null bitmaps + string heaps), and macOS should use `sysctl(HW_MEMSIZE)` (`memory/memory_resource.cpp:28`).
- `Reservation` RAII type on `MemoryResourceManager`: `MakeReservation(bytes) -> Result<Reservation>`; release on destruction.
- `Operator::EstimatePeakMemory(input_bytes)` (default 2×) + a `PipelineMemoryHistory` ring that corrects future estimates with observed peak/estimate ratios.
- Scheduler integration: clamp → reserve → run; failed reservation requeues the task (and, after phase 5, triggers a downgrade request instead of spinning).

**Done when:** with a 100 MB budget, a query whose working set is 500 MB either succeeds via spill or fails with a clean `ResourceExhausted` — never OOM-kills the process. A test asserts estimates converge (ring average within 2× of observed after 10 batches).

---

## Phase 5 — Predicate-driven spilling + batch lock states

**Sirius idea:** spilling serves reservations. `request_downgrade(predicate)` where the predicate is "does `make_reservation_or_null(bytes)` succeed now?" — a dedicated `downgrade_executor` spills *idle* batches until the predicate passes, then stops; the three-state lock (idle ⇄ read_only ⇄ mutable) on `data_batch` is what makes "idle = spillable" safe; the return trip is lazy (next task upgrades on use). Read: `src/downgrade/downgrade_executor.cpp` (526 lines) — processing thread, monitor thread, worker pool.

**Build in QUEST:**
- Three-state lock on `DataBlock`: `Idle ⇄ ReadOnly ⇄ Mutable`, with RAII accessors (`ReadLock`, `WriteLock`) — data reachable *only* through a lock, so an idle block provably has no readers.
- `DowngradeExecutor`: one background thread + `RequestDowngrade(predicate) -> std::future<size_t>`; candidates = idle blocks enumerated from `BatchHolder`s (back-to-front, like Sirius partitions); convert via existing `ToDisk()`, rollback on failure.
- Rewire: `BatchHolder::Add` stops spilling inline (`MaybeSpill` goes away); a failed reservation in the scheduler issues a downgrade request and retries when the future resolves. Add trigger/stop hysteresis (e.g. trigger 0.9, stop 0.7) so it doesn't thrash.

**Done when:** under memory pressure the executor spills the *oldest idle* blocks only, stops as soon as the pending reservation fits, never touches a locked block (assert via a test that holds a ReadLock during pressure), and a spilled block transparently upgrades on next use.

---

## Phase 6 — The blocking operators: Limit → Aggregate → Sort → Join

**Sirius idea:** every blocking operator decomposes into partition/merge stages so each task stays bounded: `HASH_GROUP_BY → PARTITION → MERGE_GROUP_BY`; ORDER_BY = three-pipeline sample sort (local sort → sample/partition → k-way merge, one partition per task); hash join = PARTITION→CONCAT on *both* sides, left = probe (streamed), right = build (materialized), build behind a FULL barrier. Read per-operator: `src/op/sirius_physical_grouped_aggregate.cpp` + `_merge`, `src/op/sirius_physical_sort_sample.cpp` / `_sort_partition` / `_merge_sort`, `src/op/sirius_physical_hash_join.cpp`, and the planner side `src/planner/sirius_plan_aggregate.cpp` / `_comparison_join.cpp`.

**Build in QUEST, in order (each = logical node + physical node + taskflow operator + tests, via the `/new-node` and `/new-op` skills):**

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
| 1 — Bridge | ☐ | first real query that spills and returns correct results |
| 2 — Pipelines | ☐ | fused pipeline beats thread-per-operator benchmark |
| 3 — Tasks | ☐ | 4-worker pool, barrier ordering test |
| 4 — Reservations | ☐ | 500 MB query under 100 MB budget: spill or clean error |
| 5 — Downgrade | ☐ | spills idle-only, stops when reservation fits |
| 6 — Operators | ☐ | TPC-H-Q1 shape + out-of-core join vs oracle |
| 7 — Vectorized | ☐ | ≥10× filter speedup, exact int64 SUM |
| 8 — Streaming | ☐ | flat memory under slow consumer; row-group pruning |
