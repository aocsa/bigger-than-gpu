# Pipeline Execution Plan — Pipelines, Not Threads-Per-Operator

**Goal:** redesign QUEST's TaskFlow DAG from thread-per-operator into a **push-oriented pipeline engine** that mimics Sirius: split the operator DAG into pipelines at blocking operators, fuse streaming operators into a single forward pass per batch, and introduce barrier semantics (`FULL` / `PIPELINE`) on cross-pipeline edges.

**Classification target** (from [`docs/sirius-push-based-event-drive.md`](../sirius-push-based-event-drive.md)): after this phase, QUEST's taskflow layer should be describable the same way as Sirius —

> producer-driven, forward batch execution inside a pipeline task; buffered holder handoffs between pipelines; event-triggered pipeline completion; and pull-like claims at buffered boundaries.

**Sirius references** (repo `/Users/aocsa/git/aocsa/sirius`, branch `docs-tutorial`):

| Concept | File | What to take |
|---|---|---|
| Meta-pipeline recursion | `src/pipeline/sirius_meta_pipeline.cpp` | streaming ops append; blocking ops end the pipeline and spawn child meta-pipelines (build side before probe side) |
| Pipeline structure | `src/pipeline/sirius_pipeline.cpp` (475) | `operators` contains *every* operator; sink pushed into the list; source = `&operators[0]`; completion predicate + status lock |
| Forward driver | `src/pipeline/gpu_pipeline_task.cpp` — `compute_task()` / `publish_output()` | the push loop: one batch through operators in plan order, then sink |
| Barrier semantics | `src/include/op/sirius_physical_operator.hpp` — `MemoryBarrierType {PIPELINE, PARTIAL, FULL}` + nested `port` | barrier lives on the consumer's port; `has_full_barrier_from()` |
| Operator roles | same header (552) | `execute()` / `sink()`+`is_sink()` / source hints — one base, four roles |
| What NOT to copy yet | `src/pipeline/sirius_pipeline_converter.cpp` (1353) | the seven-phase converter exists because of joins/partitions/CTEs; we only need its *reason*, not its machinery |

---

## 1. Current state and why it must change

Today `Graph::Execute()` ([graph.cpp:178](../../src/quest/taskflow/graph.cpp)) BFS-walks the DAG and launches **one `std::jthread` per operator**. Every edge gets a `BatchHolder`; every operator's `run()` loops `Pull → ProcessBatch → AddToOutput` until the queue finishes ([operator.hpp:392](../../src/quest/taskflow/operator.hpp), `SimpleOperator`).

Problems, in Sirius terms:

1. **No pipeline concept.** `scan >> filter >> project` pays a thread + a mutex-guarded queue handoff *per edge*, for operators that could run as one forward pass over the batch. Sirius fuses these into one `compute_task()` loop.
2. **Thread = operator, forever.** A 12-operator plan takes 12 threads regardless of cores; wide plans oversubscribe, narrow plans underuse. There is no unit of scheduling smaller than "an operator's whole lifetime."
3. **No barrier semantics.** Nothing can express "the aggregate's consumer must wait until the whole producer pipeline finished." The only tools are `ConcatenatingBatchHolder` (broken for >1 batch, [batch_holder.cpp:213](../../src/quest/cache/batch_holder.cpp)) and operator-internal waiting.
4. **Blocking a thread is the only backpressure.** `WaitIfOutputSaturated()` parks a worker inside a sink — exactly the head-of-line-blocking pattern the Sirius brief warns against (§9). The Sirius answer: express backpressure as *admission state* (don't start work), not as a parked thread.
5. **EOS via error code.** End-of-stream is signaled by `Status::Invalid` from the queue — control flow through an error value.

What we keep: the DSL (`*graph += a >> b`, named ports, `link()` + `EdgeConfig`), `Port`, `BatchHolder`/`BlockingQueue` (they become the *cross-pipeline* repositories), the memory manager wiring, and all 473 tests.

---

## 2. Target design

### 2.1 The picture

```text
Query: scan -> filter -> project -> aggregate -> sort -> collect

Today (thread-per-operator):           Target (pipelines):
  6 threads, 5 queues                    3 pipelines, holders only at breaks

  [scan]--q--[filter]--q--[project]      P0: [scan, filter, project, aggregate·sink]
     --q--[agg]--q--[sort]                        | FULL barrier
     --q--[collect]                      P1: [aggregate·source, sort·sink]
                                                  | FULL barrier
                                         P2: [sort·source, collect]
```

A **blocking operator is the sink of one pipeline and the source of the next** — it appears in both operator lists, exactly like Sirius after `finalize_pipeline_structure()`. Streaming edges *between* pipelines (rare until joins exist) keep a `BatchHolder` with a `PIPELINE` barrier.

Intra-pipeline, data moves by **push**: a `PipelineTask` owns the current `DataBlock` and forwards it through the operator list in plan order — no queue, no thread handoff, no `Pull()`. Inter-pipeline, data moves by **buffered publication**: the sink publishes into the consumer's `BatchHolder` port (or accumulates internal state for FULL-barrier blocking ops).

### 2.2 New vocabulary (all in `quest::taskflow`)

```cpp
// kpair.hpp — extends EdgeConfig
enum class BarrierType : std::uint8_t {
  Pipeline,  // consumer may run while producer is active (streaming edge)
  Full       // consumer waits until the producer pipeline finishes
};
// EdgeConfig gains: BarrierType barrier{BarrierType::Pipeline};
// EdgeConfig::Concatenating() implies Full (it already means "wait for everything").
// PARTIAL is deliberately deferred until partitioned joins exist (see §6 Non-goals).
```

```cpp
// operator.hpp — role virtuals added to Operator (mirrors sirius_physical_operator)
class Operator {
  // ... existing API unchanged ...

  // -- execute role (hoisted from SimpleOperator; default = passthrough) --
  [[nodiscard]] virtual Result<DataBlockPtr> ProcessBatch(DataBlockPtr input);

  // -- source role (sources + blocking ops acting as next-pipeline source) --
  [[nodiscard]] virtual bool IsSource() const noexcept { return false; }
  /// nullopt = exhausted (EOS is a value, not Status::Invalid)
  [[nodiscard]] virtual Result<std::optional<DataBlockPtr>> GetNextBatch();

  // -- sink role (blocking ops) --
  [[nodiscard]] virtual bool IsSink() const noexcept { return false; }
  [[nodiscard]] virtual Status Sink(DataBlockPtr batch);
  /// called once when the producer pipeline completes; flips the operator
  /// from sink-mode to source-mode (e.g. hash table built -> start emitting)
  [[nodiscard]] virtual Status FinalizeSink();
};
```

```cpp
// pipeline.hpp — NEW
class Pipeline {
 public:
  // operators_[0] is the source; back() is the sink (Sirius invariant:
  // the list contains EVERY operator, source and sink included).
  [[nodiscard]] Status RunTask(DataBlockPtr batch);   // the push loop, §2.3
  [[nodiscard]] bool IsFinished() const;              // completion predicate, §2.5
  void MarkTaskCreated();                             // BEFORE claiming input (§2.5)
  void MarkTaskCompleted();
  // dependencies: which pipelines must finish before this one may start
  [[nodiscard]] const std::vector<Pipeline*>& FullBarrierProducers() const;
 private:
  std::vector<Operator*> operators_;
  std::atomic<std::size_t> tasks_created_{0}, tasks_completed_{0};
  std::mutex status_lock_;   // load-bearing, see §2.5
};
```

```cpp
// meta_pipeline.hpp — NEW
class MetaPipeline {
 public:
  /// Walks the finalized Graph and splits it into pipelines at blocking
  /// operators. Child meta-pipelines are built before their parent is
  /// scheduled (Sirius: build side before probe side).
  [[nodiscard]] static Result<std::vector<PipelinePtr>> Build(Graph& graph);
};
```

```cpp
// pipeline_executor.hpp — NEW (replaces Graph::Execute's thread-per-operator)
class PipelineExecutor {
 public:
  explicit PipelineExecutor(std::size_t num_workers /* default hw_concurrency */);
  [[nodiscard]] Status Execute(std::vector<PipelinePtr> pipelines);
};
```

### 2.3 The push loop (mimics `gpu_pipeline_task::compute_task`)

```cpp
Status Pipeline::RunTask(DataBlockPtr batch) {
  // forward pass: operators_[1] .. operators_[n-2] are streaming
  for (std::size_t i = 1; i + 1 < operators_.size(); ++i) {
    auto result = operators_[i]->ProcessBatch(std::move(batch));
    if (!result) return result.error();
    batch = std::move(result.value());
    if (batch->num_rows() == 0) return Status::OK();  // filter ate everything
  }
  return PublishOutput(std::move(batch));  // sink() — accumulate or publish
}
```

`PublishOutput` dispatches on the sink: a blocking operator gets `Sink(batch)` (accumulate state); a boundary to a `PIPELINE`-barrier consumer gets `AddToOutput` into the cross-pipeline `BatchHolder` (existing spill-aware path — `MaybeSpill` keeps working unchanged); the terminal collector gets the batch directly.

Note the classification consequence (brief §3): the operator *returning* a batch does not make this pull — the driver already had the batch and forwards it. No consumer ever calls upstream.

### 2.4 Meta-pipeline build algorithm

Input: the finalized `Graph` (DSL unchanged — users still write `*graph += scan >> filter >> agg`). Output: pipelines + dependency edges.

```text
BuildPipeline(sink_op, meta):
  current = new Pipeline, ending at sink_op
  walk upstream via GetIncomingEdges():
    streaming op          -> prepend to current, keep walking
    source op             -> prepend; current is complete
    blocking op B         -> B becomes current's source (position 0);
                             recurse: BuildPipeline(B, meta)  # B is that pipeline's sink
                             record FULL dependency: current depends on B's pipeline
    PIPELINE-barrier edge -> stop fusing; upstream keeps its own pipeline;
                             the edge keeps its BatchHolder (buffered handoff)
  multi-input op (join, later): recurse per input port,
                             build-side child pipelines registered BEFORE probe side
```

Blocking classification comes from `OperatorType` via one function in `operator_type.hpp` (`IsBlockingOperator`): `ComputeAggregate`, `SortAndSample`, `MergeStream`, `PartwiseJoin`, `JoinPartition`, `Union` → blocking; `TableScan` → source; `Filter`, `Project`, `Limit`, `Output` → streaming. An `Operator::IsBlocking()` virtual consults it (overridable, like Sirius's `build_pipelines`).

Holder placement rule (Sirius: "repositories connect sink→source, never mid-pipeline"): **intra-pipeline edges get no BatchHolder at all** — `AddEdge` still records the topology, but holder creation moves from `AddEdge` to `MetaPipeline::Build`, which allocates holders only for cross-pipeline edges.

### 2.5 Completion, the status lock, and events

Straight from the brief (§8, §12.4) — these are the invariants that make the design correct, and they must be built in from day one:

- **Completion predicate:** a pipeline is finished ⇔ its source is exhausted **and** its input holders are empty **and** `tasks_created == tasks_completed`.
- **The task-creation lock is load-bearing:** claim input → construct task → `MarkTaskCreated()` must happen under `status_lock_`, or a completion check can observe empty ports + balanced counters while a task exists conceptually but isn't counted yet. (Sirius shipped this race; we inherit the fix, not the bug.)
- **Completion is edge-triggered:** `MarkTaskCompleted()` runs the status check; if the pipeline finished, it fires the completion event. Nothing polls. Any future condition that can become true without a task completing (streaming EOS, cancellation) must inject its own wake — document this on `PipelineExecutor` now.
- **Error teardown order:** stop creating pipeline work → drain queued work → join workers. Queued tasks hold raw `Operator*` owned by the plan (brief §12.5).

`PipelineExecutor` event loop (deliberately minimal — batch-granular tasks and the hint chain are the *next* phase):

```text
runnable(P) := all FullBarrierProducers(P) finished
on start:        schedule every runnable pipeline onto the worker pool
worker(P):       loop { claim next source batch under status lock;
                        none -> break; RunTask(batch); }
                 on source exhausted + tasks drained -> FinalizeSink(P.sink),
                 mark finished, emit completion event
on completion:   re-evaluate runnable set, schedule newly-runnable pipelines
PIPELINE edges:  producer and consumer pipelines may run concurrently;
                 consumer's source claims from the shared BatchHolder
                 (pull-like claim at a buffered boundary — that's the hybrid)
```

Workers = pool of N (`hw_concurrency` default), not one per operator. A plan with 12 operators in 3 pipelines uses ≤3 concurrent workers before phase-next parallelism (multiple tasks per pipeline) even exists.

---

## 3. Implementation stages

Each stage compiles, passes all existing tests plus its own, and is a reviewable commit. Follow house rules: `run()`/`can_you_throttle_my_input()` on every operator, tests registered in `tests/CMakeLists.txt`, `Result`/`Status` (no exceptions), plan in `tasks/todo.md` before starting.

### Stage 0 — Read Sirius, write the note (no code)
Read `sirius_meta_pipeline.cpp`, `sirius_pipeline.cpp` (`update_pipeline_status`), `gpu_pipeline_task.cpp` (`compute_task`/`publish_output`), and the `port`/`MemoryBarrierType` section of `sirius_physical_operator.hpp`. Write ≤1 page in `tasks/notes/pipelines.md`: *how does one batch cross a pipeline break in Sirius?* If the note can't answer where the FULL barrier is checked, re-read.

### Stage 1 — Barrier vocabulary
- [ ] `BarrierType {Pipeline, Full}` on `EdgeConfig`; `Concatenating()` sets `Full`; `link()` plumbs it.
- [ ] `IsBlockingOperator(OperatorType)` in `operator_type.hpp` + `Operator::IsBlocking()` virtual.
- [ ] Tests: EdgeConfig defaults, DSL round-trip (`link(a, b, EdgeConfig::Concatenating())` yields a Full edge).

### Stage 2 — Operator roles
- [ ] Hoist `ProcessBatch` to `Operator` (default passthrough); `SimpleOperator` keeps its `run()` so the legacy path still works during migration.
- [ ] Add `IsSource`/`GetNextBatch` (EOS = `std::nullopt`, retiring `Status::Invalid`-as-EOS on the new path), `IsSink`/`Sink`/`FinalizeSink`.
- [ ] Test doubles in `tests/`: `VectorSourceOperator` (yields N synthetic batches), `AccumulatingSinkOperator` (a mock blocking op: `Sink` collects, `FinalizeSink` flips it to re-emit — a stand-in for sort/aggregate until phase 6 builds real ones), `CountingStreamOperator`.

### Stage 3 — `Pipeline` + `MetaPipeline::Build`
- [ ] `pipeline.{hpp,cpp}`: operator list, counters, `status_lock_`, completion predicate, `RunTask` push loop.
- [ ] `meta_pipeline.{hpp,cpp}`: the §2.4 walk. Holder creation moves out of `AddEdge` into `Build` (cross-pipeline edges only).
- [ ] Shape tests (build only, no execution): linear plan → 1 pipeline; `scan >> filter >> agg >> project >> collect` → 2 pipelines with agg in both lists + FULL dependency; two sources into a two-port mock sink → build-side pipeline registered before probe-side; PIPELINE-barrier edge → 2 pipelines, holder present, no FULL dependency.

### Stage 4 — `PipelineExecutor`
- [ ] `pipeline_executor.{hpp,cpp}`: worker pool, runnable-set evaluation, completion events, claim-under-lock, error teardown order (§2.5).
- [ ] `Graph::ExecutePipelined()` = `MetaPipeline::Build` + `PipelineExecutor::Execute`. Old `Execute()` stays untouched until Stage 6.
- [ ] Tests: FULL gating (consumer provably starts after producer finishes — assert via timestamps); PIPELINE concurrency (producer and consumer overlap — assert consumer receives first batch while producer still active); completion race (loop the claim/complete sequence under TSAN-style stress: many small batches, assert no premature finish); error propagation (mid-pipeline `ProcessBatch` failure stops the query, teardown order holds); worker-pool bound (12-op plan runs correctly with 2 workers).

### Stage 5 — Correctness parity + benchmark
- [ ] Parity test: same mock plan through `Execute()` (old) and `ExecutePipelined()` (new) → identical batches.
- [ ] Spill still works: tiny CPU budget, cross-pipeline holder spills to disk, results correct.
- [ ] Benchmark (record numbers in the test file): `scan >> filter >> project >> collect` over ~10M synthetic rows, fused pipeline vs thread-per-operator. Expected: fewer threads, fewer queue handoffs, measurable win.
- [ ] `/verify-operator` on the new driver path; `/review-mem` on Pipeline/Executor (raw `Operator*` lifetimes: executor must never outlive the plan).

### Stage 6 — Flip the default
- [ ] `Graph::Execute()` delegates to the pipelined path; legacy BFS retired (or kept behind a flag for one release of tests, then deleted — don't maintain two schedulers).
- [ ] Update `docs/dev/ARCHITECTURE.md` execution-model section + the architecture page.
- [ ] Review section in `tasks/todo.md`; lessons to `tasks/lessons.md`.

---

## 4. Done when

1. `scan >> filter >> project >> collect` executes as **one pipeline**: one worker, zero intermediate BatchHolders, verified by pipeline-count and holder-count assertions.
2. A plan with a mock blocking operator runs as two pipelines where the consumer **cannot** observe the producer unfinished (FULL), and a PIPELINE-edge plan shows genuine producer/consumer overlap.
3. Completion is event-triggered and race-free under stress (the §2.5 lock discipline has a dedicated test).
4. All pre-existing tests green; parity test proves old ≡ new; benchmark recorded.
5. The review checklist from the brief (§13) has a precise answer for every question when asked of QUEST — that's the definition of having actually mimicked the model.

## 5. Risks / decisions taken

- **Blocking ops don't exist yet.** True — phase 6 of the [replication plan](sirius-replication-plan.md) builds them. This phase ships the *seams* (roles, barriers, meta-pipeline) proven by mocks, so aggregate/sort/join later slot into a working chassis instead of forcing a redesign. This mirrors how Sirius's converter phases exist independently of any single operator.
- **One task = one source batch claim, sequential per pipeline.** Intra-pipeline parallelism (multiple concurrent tasks per pipeline) is deferred to the task-creator phase — it needs the hint chain and per-port claim discipline to be worth doing. The `tasks_created/completed` counters are built now so that phase is additive.
- **`ConcatenatingBatchHolder` stays broken** until a FULL-barrier consumer actually needs multi-batch concat (the mock sink accumulates internally). Fix lands with the first real blocking operator.
- **Operators keep raw pointers + external ownership** (current model: caller owns operators, `Graph` holds `Operator*`). Changing ownership is orthogonal; noted for `/review-mem`.

## 6. Non-goals (explicitly deferred)

- `PARTIAL` barriers, partition/merge operators, sibling-linked partitions — arrive with joins (replication plan phase 6).
- Task-creator hint chain, batch-granular scheduling, device/NUMA preferences — next phase (replication plan phase 3); this design leaves the counters, locks, and events it needs.
- Memory reservations before dispatch (phase 4) and predicate-driven downgrade (phase 5).
- Compiling/fusing expressions inside an operator (replication plan phase 7) — orthogonal to pipeline structure, as the brief notes (§3).
