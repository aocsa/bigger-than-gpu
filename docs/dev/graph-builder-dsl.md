# GraphBuilder DSL — Fluent Builder + Runtime Edge Tuning (Design)

**Status:** design + implementation plan (Phase 2.5). This documents Option 3 from the
DSL discussion (fluent builder) in full, plus the runtime edge-tuning API that
motivates it. **Phase 2.5 replaces the old taskflow wiring DSL** (`>>` / `link()` /
`Graph::operator+=`) — it is not additive. Revised for forward compatibility with
replication phases 3 (task-creator hint chain), 5 (predicate-driven spilling), and 6
(blocking operators) — see §10 for the readiness matrix and §9 for the recorded
decisions.

**Why a builder at all:** the old DSL split into two idioms — `a >> b` for plain
edges and `link(a, b, EdgeConfig::...)` for anything custom — and the custom path is
where the bugs lived (the P1 review finding: `CreateGraph` forgot the collector's Full
barrier and no query could spill; a typo'd port name like `join["biuld"]` silently
created a new port). A builder gives one discoverable API, a natural validation
point (`Build()`), and — the real driver — **stable edge identity plus a lifecycle**
that makes tuning edges *after* construction sound. Flow-control thresholds, partition
counts, and spill behavior can only be chosen once Parquet footer statistics, data
sizes, and hardware resources are known — which is later than graph construction, and
sometimes during execution.

---

## 1. Goals / non-goals

**Goals**
1. One API that covers every case the old DSL covered (plain edges, named ports, holder
   type, barrier, partitions, flow control) with verbosity proportional to deviation.
2. Every edge individually addressable **after** construction — by name or endpoints —
   so configs can be tuned when statistics arrive. This includes edges the author
   never wrote (phase 6 will synthesize partition/merge structure — §6.2).
3. A clear lifecycle defining *what* is tunable *when*, expressed as **types**, not
   runtime state checks: structural tuning lives on `Graph`, soft tuning on
   `PreparedQuery` (§2).
4. Extensible without modifying the builder: a new operator ships its own port
   contracts and wiring helpers.
5. `Build()`-time validation: declared ports, acyclicity, contract conflicts.
6. Knob *semantics* defined abstractly enough to survive the engine's evolution:
   flow control is a **task-admission condition** (not "block a thread" — phase 3),
   spill policy is a **candidate-eligibility hint** (not "serialize on Add" — phase 5).
7. **Replace** the old taskflow wiring DSL: after Phase 2.5, `GraphBuilder` is the
   single supported graph-wiring API. `>>`, `link()`, and `Graph::operator+=` are
   deleted (not kept as a parallel surface).

**Non-goals**
- Changing the logical-plan fluent API (`QueryBuilder::FromParquet.Where.Select`).
  That layer stays; only the taskflow graph-wiring layer is replaced.
- Changing pipeline/executor semantics ahead of their phases. `MetaPipeline::Build`
  and `PipelineExecutor` read the same `Graph` + `EdgeConfig` they read today.
- Distributed/multi-node concerns.

---

## 2. The lifecycle model (what makes runtime tuning sound)

The current `Graph::AddEdge` creates `BatchHolder`s **eagerly** at wiring time. That
forecloses post-construction tuning: by the time you have footer stats, the holders
already exist with baked-in thresholds. The builder therefore defers materialization —
which also retires the known Stage-6 debt ("holders stay in AddEdge until the legacy
path dies").

**Decision (was open question 2): `Prepare()` produces a new object, `PreparedQuery`.**
The state machine becomes types instead of runtime checks:

```
GraphBuilder ──Build()──► Graph                ──Prepare(stats,hw)──► PreparedQuery ──Execute()──► results
                          │                                            │
                          │ STRUCTURAL TUNING                          │ SOFT TUNING (any time,
                          │ barrier, holder type,                      │ incl. mid-execution):
                          │ partitions/bounds,                         │ flow control, spill
                          │ flow control, spill policy                 │ policy, task admission
                          │ (no holders exist)                         │ (holders + pipelines live)
                          │
                          └── reusable: Prepare() again ──► another PreparedQuery
                              (same shape, different stats/tuning)
```

| Object | Owns | Mutable surface |
|---|---|---|
| `Graph` (Built) | operators (non-owning) + **edge specs** — no holders | everything: `Tune()`, `AddTuningPolicy()` |
| `PreparedQuery` | resolved configs, holders, pipelines, executor | **soft knobs only**: flow control, spill policy, per-pipeline task admission |

Why the type split beats state flags: structural retuning on a `PreparedQuery` is a
compile error, not a runtime `Status::Invalid`; and one `Graph` can be prepared
repeatedly — same query shape re-run tomorrow against a bigger file gets fresh
stats-driven configs for free.

Three **tunability classes** (the third exists because of phase 6 — §10):

| Class | Examples | Bound when |
|---|---|---|
| Structural | barrier, holder type, fixed partition count | frozen at `Prepare()` |
| **Late-bound structural** | actual partition count within `PartitionedUpTo(max)` | declared bounds at `Prepare()`; **bound by the operator during execution** when real input sizes are known (the Sirius `determine_num_partitions` pattern) |
| Soft | flow-control thresholds, spill policy, per-pipeline max in-flight tasks | any time, atomically |

---

## 3. GraphBuilder API reference

Everything lives in `quest::taskflow`. All operators are non-owning `Operator*` into
caller-owned storage, exactly like `Graph` today; `Build()` documents the lifetime
contract.

### 3.1 Construction

```cpp
class GraphBuilder {
 public:
  explicit GraphBuilder(
      std::shared_ptr<memory::MemoryResourceManager> memory_manager = nullptr);
```

### 3.2 Linear chains — the 90% case

**Decision (was open question 1): `Chain()` returns a `ChainBuilder` whose fluent
refinements apply to the chain's *last* edge.** The rule is one sentence —
"refinement after `Chain()` targets its final edge" — and it covers the overwhelmingly
common case (a chain ending at a collector/blocking consumer) without reintroducing
precedence puzzles. Mid-chain custom edges still use `Edge()`.

```cpp
  /// Wires plain (streaming, fusable) edges between consecutive operators.
  /// Chain(a, b, c) == Edge(a, b) + Edge(b, c). The returned ChainBuilder
  /// exposes the full EdgeBuilder vocabulary, applied to the LAST edge only:
  ///   gb.Chain(scan, filter, project, collector).Full();   // project→collector is Full
  template <typename... Ops>                    // Operator& or OperatorPortPair
  ChainBuilder Chain(Ops&... ops);
```

### 3.3 Single edge with fluent config

```cpp
  /// Starts (or amends) one edge. The returned EdgeBuilder mutates the edge
  /// spec in place — no commit step; calling Edge/Chain/Build next is safe.
  EdgeBuilder Edge(Operator& src, Operator& dst);
  EdgeBuilder Edge(Operator& src, OperatorPortPair dst);   // e.g. join["build"]
  EdgeBuilder Edge(OperatorPortPair src, Operator& dst);
  EdgeBuilder Edge(OperatorPortPair src, OperatorPortPair dst);
```

```cpp
enum class SpillPolicy : std::uint8_t {
  Eligible,   // default: this edge's buffered batches may spill
  Never,      // exempt from spilling (latency-critical, tiny)
  First       // preferred spill victim (large, cold — e.g. a join build side)
};

class EdgeBuilder {                 // returned by Edge(); refers to ONE edge spec
 public:
  // --- barrier semantics -------------------------------------------------
  EdgeBuilder& Full();              // consumer waits for producer pipeline
  EdgeBuilder& Streaming();         // BarrierType::Pipeline (the default)
  EdgeBuilder& Partial();           // RESERVED for phase 6 (partitioned
                                    // incremental consumption); Build() errors
                                    // until BarrierType::Partial exists

  // --- holder type / partitioning -----------------------------------------
  EdgeBuilder& Concatenating();     // merge-all holder; implies Full()
  EdgeBuilder& Partitioned(std::size_t partition_count);      // fixed at Prepare
  EdgeBuilder& PartitionedUpTo(std::size_t max_partitions);   // late-bound: the
                                    // operator binds the ACTUAL count (≤ max) at
                                    // runtime when input sizes are known (§10, phase 6)

  // --- backpressure & spilling ----------------------------------------------
  /// SEMANTICS (phase-stable): an admission condition on the edge's buffer —
  /// "don't admit more producer work while the holder holds ≥ max". Today it
  /// blocks the producer in WaitIfSaturated(); after phase 3 it gates task
  /// creation instead. The DSL surface does not change.
  EdgeBuilder& FlowControl(std::size_t max_batches,
                           std::size_t max_bytes = kUnlimited);
  /// SEMANTICS (phase-stable): an eligibility/priority hint for spilling.
  /// Today: Never maps to auto_spill=false on the holder. After phase 5 it is
  /// consumed by the downgrade executor's candidate enumeration (Never =
  /// skipped, First = front of the victim order).
  EdgeBuilder& Spill(SpillPolicy policy);

  // --- identity & escape hatch ---------------------------------------------
  EdgeBuilder& Named(std::string edge_name);   // stable handle for tuning (§6)
  EdgeBuilder& Config(EdgeConfig config);      // replace the whole config

  // --- continuation ---------------------------------------------------------
  EdgeBuilder ThenTo(Operator& next);
  EdgeBuilder ThenTo(OperatorPortPair next);
  GraphBuilder& Done();             // explicit return to the builder (optional sugar)
};

class ChainBuilder : public EdgeBuilder { /* bound to the chain's last edge */ };
```

Design choice worth calling out: `EdgeBuilder` holds a pointer to the edge *spec*
inside the builder and mutates it directly. There is no deferred "commit" (no
destructor magic) — the spec is registered the moment `Edge()` returns, and later
fluent calls refine it. This keeps partially-configured edges impossible to lose.

### 3.4 Composite shapes — the extension point

```cpp
  /// Applies a reusable topology pattern. Any type with
  ///   void WireInto(GraphBuilder&)
  /// works — shapes ship WITH their operators, never inside the builder.
  template <typename Shape>
  GraphBuilder& Apply(Shape&& shape);
```

### 3.5 Build

```cpp
  /// Validates and produces the graph. Consumes the builder.
  /// Validation: (a) every named port the consumer DECLARES exists — typos
  /// like join["biuld"] become errors, not silent new ports; (b) the edge set
  /// is acyclic; (c) explicit configs don't contradict a consumer's declared
  /// port contract (see §5) unless .Config() was used as an explicit override;
  /// (d) duplicate operator ids / duplicate edge names rejected; (e) reserved
  /// vocabulary (.Partial()) rejected until its phase lands.
  ///
  /// The result is a Graph in the **Built** state: edge specs recorded, NO
  /// holders created. Holders materialize in Prepare() -> PreparedQuery (§6).
  [[nodiscard]] Result<std::shared_ptr<Graph>> Build() &&;
```

---

## 4. Extensibility — adding a new operator to the DSL

Three mechanisms, in increasing order of sugar. None require touching
`GraphBuilder`. Phase 6's operators (Limit, hash Aggregate, sample Sort, partitioned
hash Join) are the intended first consumers — each ships all three pieces.

### 4.1 Port contracts (the semantic layer)

The operator declares what its input ports *require*; the builder honors it whenever
the user didn't say otherwise:

```cpp
class Operator {
  /// Ports this operator accepts (empty = undeclared, any name allowed —
  /// legacy behavior). Build() validates edge targets against this.
  [[nodiscard]] virtual std::vector<std::string_view> DeclaredInputPorts() const;

  /// The EdgeConfig this operator requires on a given input port.
  [[nodiscard]] virtual EdgeConfig InputEdgeContract(std::string_view port) const;
};
```

A new `HashJoinOperator` then makes *every* wiring site correct by construction:

```cpp
std::vector<std::string_view> DeclaredInputPorts() const override {
  return {"probe", "build"};
}
EdgeConfig InputEdgeContract(std::string_view port) const override {
  if (port == "build") return EdgeConfig::Concatenating();   // implies Full
  return {};                                                  // probe streams
}
```

This is the Sirius placement: barrier semantics are decided by the code that knows
the operator (`sirius_physical_operator`'s `port{FULL,...}` is created by operator
logic), never by the query author.

### 4.2 Typed port accessors (kill stringly-typed ports)

```cpp
class HashJoinOperator : public Operator {
 public:
  [[nodiscard]] OperatorPortPair probe() { return {this, "probe"}; }
  [[nodiscard]] OperatorPortPair build() { return {this, "build"}; }
};
// usage: gb.Edge(orders_scan, join.probe());   — a typo is now a compile error
```

### 4.3 Shape helpers (topology patterns as values)

```cpp
// ships in hash_join_operator.hpp, NOT in graph_builder.hpp
struct HashJoinShape {
  Operator& probe_source;
  Operator& build_source;
  HashJoinOperator& join;

  void WireInto(GraphBuilder& gb) {
    gb.Edge(probe_source, join.probe());
    gb.Edge(build_source, join.build())     // contract supplies Concatenating+Full
      .Spill(SpillPolicy::First);           // big cold build side: preferred victim
  }
};

// usage:
gb.Apply(HashJoinShape{orders_scan, customers_scan, join});
```

Phase 6's sample sort follows the same recipe (`SortShape` wiring
sample→partition→merge with `PartitionedUpTo(...)` on the exchange edge). The
builder never changes.

---

## 5. Config resolution — one precedence chain

Every edge's *effective* config is resolved at `Prepare()` time (when stats exist),
from highest to lowest priority:

```
1. Live tuner            (PreparedQuery::Set* during execution — soft knobs only)
2. Plan-time retune      (Tune() / the TuningPolicy CHAIN, after Build, at Prepare)
3. Explicit builder call (.Full(), .FlowControl(...), .Config(...))
4. Consumer port contract (InputEdgeContract)
5. EdgeConfig{} defaults  (Pipeline barrier, Simple holder, unlimited flow, Spill::Eligible)
```

Two rules that keep surprises out:

- **Policies refine, they don't fight.** Layer-2 policies receive the layer-3/4
  resolution as input (`EdgeInfo::config`) and may return it unchanged.
- **Resolution runs AFTER synthesis.** Phase 6 will inject operators and edges the
  author never wrote (partition/merge stages, relay pipelines — the Sirius converter
  pattern). `Prepare()`'s internal order is therefore fixed as:
  `synthesize structure → resolve configs (policy chain sees the FINAL edge set,
  synthesized edges included) → materialize holders`. A policy keyed on "edges into
  a merge operator" works without knowing whether the author or the planner created
  them.

Late-bound structural knobs (`PartitionedUpTo`) sit deliberately *outside* this
chain: the chain fixes their **bounds** at `Prepare()`; the owning operator binds the
actual value during execution, within those bounds.

---

## 6. Runtime tuning API

### 6.1 Identity: addressing an edge after construction

```cpp
struct EdgeRef {
  std::int32_t source_id;
  std::int32_t target_id;
  std::string  target_port;   // "" = default port
};
// or by the stable name given at build time:
gb.Edge(project, collector).Full().Named("into_collector");
```

**Synthesized edges** (phase 6) get generated, stable, documented names:
`"<consumer_name>_<consumer_id>.<port>~synth<k>"` (e.g. `hash_join_5.build~synth0`
for the injected CONCAT→build edge). They appear in `Edges()` like any other edge and
are tunable through the same APIs — which is precisely why the policy hook (§6.4)
is the recommended tuning mechanism: it sees them; hand-written `Tune("name")` calls
generally won't know they exist.

### 6.2 Introspection

```cpp
struct EdgeInfo {
  EdgeRef      ref;
  std::string  name;                 // author-given, or generated for synthesized edges
  bool         synthesized;          // false = author-written
  EdgeConfig   config;               // resolved so far (layers 3–5)
  OperatorType source_type, target_type;
};
struct PipelineInfo {                // phase 3 makes pipelines a tuning surface too
  std::size_t  pipeline_id;
  std::vector<std::int32_t> operator_ids;
  std::size_t  max_in_flight_tasks;  // task-admission bound (see §6.5)
};

[[nodiscard]] std::vector<EdgeInfo>     Graph::Edges() const;          // pre-synthesis view
[[nodiscard]] std::vector<EdgeInfo>     PreparedQuery::Edges() const;  // FINAL view, synthesized incl.
[[nodiscard]] std::vector<PipelineInfo> PreparedQuery::Pipelines() const;
```

### 6.3 Structural retuning — on `Graph` only (compile-time enforced)

```cpp
/// Fluent, same vocabulary as EdgeBuilder. Lives on Graph; PreparedQuery has
/// no such method — structural retuning after Prepare is a compile error,
/// not a runtime check.
[[nodiscard]] Result<EdgeTuner> Graph::Tune(const EdgeRef&);
[[nodiscard]] Result<EdgeTuner> Graph::Tune(std::string_view edge_name);

graph->Tune("into_collector").value()
     .PartitionedUpTo(hw.cores)
     .FlowControl(64, ram_budget / active_edges);
```

### 6.4 The policy chain — tune when the stats arrive, declaratively

**Decision (was open question 3): a policy CHAIN, not a single policy.** The phase
review settles this: by phase 6 at least three independent concerns tune edges —
flow-control sizing (this design), spill priorities (phase 5), and partition bounds
(phase 6) — typically authored by different modules (the planner, the memory
subsystem, the operator library). One `std::function` would force a single module to
know everything. Policies are applied in registration order, each receiving the
previous output; last write wins per field.

```cpp
struct QueryStats {                  // from Parquet footers / DataSource metadata
  std::size_t estimated_rows{0};
  std::size_t estimated_bytes{0};
  std::size_t row_group_count{0};
};
struct HardwareInfo {                // probed once per process
  std::size_t cores{0};
  std::size_t ram_bytes{0};
  std::size_t disk_free_bytes{0};
};

using EdgeTuningPolicy = std::function<EdgeConfig(
    const EdgeInfo& edge, const QueryStats& stats, const HardwareInfo& hw)>;

void Graph::AddTuningPolicy(EdgeTuningPolicy policy);   // chainable, ordered

/// Synthesizes converter structure, resolves every edge through §5's chain
/// (running the policy chain per edge, synthesized edges included), creates
/// the holders, builds pipelines — and hands them to a PreparedQuery.
/// Callable repeatedly: each call yields an independent PreparedQuery.
[[nodiscard]] Result<PreparedQuery> Graph::Prepare(const QueryStats& stats,
                                                   const HardwareInfo& hw);
```

Example — two policies from two modules, composed:

```cpp
// Registered by the memory subsystem:
graph->AddTuningPolicy([](const EdgeInfo& e, const QueryStats& s, const HardwareInfo& hw) {
  EdgeConfig cfg = e.config;
  if (cfg.barrier == BarrierType::Full) {
    cfg.flow_control_bytes_threshold = std::min(s.estimated_bytes, hw.ram_bytes / 4);
    cfg.flow_control_batches_threshold = std::max<std::size_t>(4, s.row_group_count);
  }
  return cfg;
});
// Registered by the operator library (phase 6):
graph->AddTuningPolicy([](const EdgeInfo& e, const QueryStats& s, const HardwareInfo& hw) {
  EdgeConfig cfg = e.config;
  if (cfg.max_partitions > 1)   // late-bound bound-tightening
    cfg.max_partitions = std::clamp<std::size_t>(s.estimated_bytes >> 29, 1, hw.cores);
  return cfg;
});
```

### 6.5 Live tuning — on `PreparedQuery`, soft knobs only

```cpp
class PreparedQuery {
 public:
  [[nodiscard]] Status Execute();          // blocking; runs the PipelineExecutor

  // --- per-edge soft knobs (any time, incl. mid-execution, atomic) ---------
  [[nodiscard]] Status SetFlowControl(const EdgeRef&, std::size_t max_batches,
                                      std::size_t max_bytes);
  [[nodiscard]] Status SetSpillPolicy(const EdgeRef&, SpillPolicy);

  // --- per-pipeline task admission (phase 3 surface, reserved now) ----------
  /// Bounds concurrent tasks minted for one pipeline. Meaningless until the
  /// task-creator phase (today: one drain per pipeline); reserved so adaptive
  /// controllers written against PreparedQuery survive the engine swap.
  [[nodiscard]] Status SetMaxInFlightTasks(std::size_t pipeline_id, std::size_t n);

  // --- bulk pressure relief (decision, was open question 4) -----------------
  /// Multiplies every edge's flow-control thresholds by `factor` (clamped to
  /// ≥1 batch). Thin sugar over Edges() + SetFlowControl — shipped because
  /// adaptive controllers (the §8.6 watchdog) want one atomic-ish lever, but
  /// documented as a STOPGAP: phase 5's downgrade executor is the principled
  /// pressure-relief mechanism, and the watchdog pattern retires with it.
  [[nodiscard]] Status ScaleFlowControl(double factor);
};
```

**Phase-3 wake contract (load-bearing).** Today a raised threshold re-wakes producers
via `NotifySaturationWaiters()` (they block in `WaitIfSaturated()`). After phase 3
**nobody blocks** — a full holder simply means the producer's task hint answers
WAITING and no task is created. A live threshold change must therefore also post a
**re-evaluation event** to the task creator/scheduler, or the change sits inert until
some unrelated completion happens to re-run the hint — exactly the edge-triggered
completion trap the push/event-driven brief (§8) warns about: *a correct predicate is
insufficient if no event invokes it.* Implementation note §7.2 carries this.

---

## 7. Implementation notes (for whoever builds this)

1. **Deferred holders + PreparedQuery.** `GraphBuilder::Build()` stores edge specs on
   the Graph; `Graph::Prepare()` synthesizes, resolves (§5), materializes holders,
   builds pipelines, and moves them into a `PreparedQuery` that owns the executor.
   `Graph::Execute()` becomes sugar: `Prepare({}, {}).Execute()`. `ExecuteLegacy()`
   keeps eager semantics for its remaining callers until deletion.
2. **Live knobs need atomics AND a wake.** `BatchHolderConfig`'s thresholds
   (`batch_holder.hpp:32-35`) become `std::atomic` on the holder;
   `SetFlowControl` writes them, calls `NotifySaturationWaiters()` (today's blocked
   producers), **and** — once phase 3 lands — posts a task-creation re-evaluation
   event (§6.5). `SpillPolicy` is an `std::atomic<SpillPolicy>` per holder; today
   `Never` maps to `auto_spill=false`, after phase 5 the downgrade executor's
   candidate provider reads it during enumeration (Never = skip, First = victim-order
   front).
3. **Synthesis hook.** `Prepare()` calls a (initially empty) `SynthesizeStructure()`
   step before resolution — phase 6's converter logic (partition/merge injection,
   relay pipelines) plugs in there, and generated edges get §6.1's naming scheme.
4. **Names.** `edge_names_: map<string, EdgeRef>`; duplicates rejected at Build();
   synthesized names generated deterministically so re-Prepare yields the same names.
5. **Estimated sizes.** Builder + Edge/ChainBuilder ≈ 250 lines; Graph spec storage +
   policy chain ≈ 150; PreparedQuery ≈ 250; holder atomics + wake ≈ 40; tests
   (validation, precedence, chain-last-edge rule, policy ordering, live re-wake,
   re-Prepare independence) ≈ 500.
6. **Ordering with the roadmap.** Land this *before* replication phase 4 (memory
   reservations): `Prepare(stats, hw)` is exactly where reservation estimates plug in,
   and `QueryStats`/`HardwareInfo` are shared vocabulary. Phase 3 then swaps the
   executor underneath `PreparedQuery` without touching the DSL; phases 5/6 consume
   the hooks in notes 2–3.

---

## 8. Migration gallery — old DSL → builder

All "old" snippets were real call sites from `tests/test_manual_query_walkthrough.cpp`,
`tests/test_graph.cpp`, and `tests/test_meta_pipeline.cpp`. They are a **migration
guide**: after Phase 2.5 the left-hand forms are gone; only the builder forms remain.

### 8.1 Linear query with a collector (walkthrough Stage 4)

```cpp
// OLD — two idioms; the Full barrier is the author's job to remember
// (forgetting it was the P1 bug: no query could spill):
*graph += scan_op >> filter_op;
*graph += filter_op >> project_op;
*graph += link(project_op, collector_op,
               EdgeConfig::Simple().with_barrier(BarrierType::Full));

// NEW — with CollectorOperator declaring InputEdgeContract() = Full:
auto graph = GraphBuilder{memory_manager}
                 .Chain(scan_op, filter_op, project_op, collector_op)
                 .Build();                    // Full inferred at the collector

// NEW — explicit, using the chain-last-edge rule (decision Q1):
auto graph = GraphBuilder{memory_manager}
                 .Chain(scan_op, filter_op, project_op, collector_op)
                 .Full().Named("into_collector")        // applies to project→collector
                 .Done().Build();
```

### 8.2 Concatenating edge with flow control (`test_graph.cpp`)

```cpp
// OLD:
*graph += link(source, sink,
               EdgeConfig::Concatenating().with_flow_control(10, 1024 * 1024));

// NEW:
gb.Edge(source, sink).Concatenating().FlowControl(10, 1_MiB);
```

### 8.3 Two-input join (`test_graph.cpp`'s JoinOperator)

```cpp
// OLD — stringly-typed ports; a typo silently creates a new port:
*graph += table_a >> join["left_input"];
*graph += table_b >> join["right_input"];
*graph += join >> output;

// NEW — typed ports + contract (build side gets Concatenating+Full for free):
gb.Edge(table_a, join.probe());
gb.Edge(table_b, join.build()).Spill(SpillPolicy::First);
gb.Chain(join, output);

// NEW, shape form:
gb.Apply(HashJoinShape{table_a, table_b, join}).Chain(join, output);
```

### 8.4 Partitioned exchange (`test_meta_pipeline.cpp`)

```cpp
// OLD — the author guesses a count at wiring time:
*graph += link(scan, worker, EdgeConfig::Partitioned(2));

// NEW — declare BOUNDS, let stats (policy) tighten them and the operator
// bind the actual count at runtime (late-bound structural, §2):
gb.Edge(scan, worker).PartitionedUpTo(64).Named("exchange");
```

### 8.5 Diamond — one blocking producer, two consumers

```cpp
// OLD:
*graph += scan >> agg;
*graph += agg >> project_a;
*graph += agg >> project_b;

// NEW:
gb.Chain(scan, agg);                          // agg's contract handles the break
gb.Edge(agg, project_a);
gb.Edge(agg, project_b);
```

### 8.6 Kitchen sink — build once, prepare per run, adapt live

```cpp
// -- plan time: wire the SHAPE, defer the NUMBERS --------------------------
GraphBuilder gb{memory_manager};
gb.Apply(HashJoinShape{orders_scan, customers_scan, join});
gb.Chain(join, filter, project, collector).Full().Named("into_collector");
ASSERT_OK_AND_ASSIGN(auto graph, std::move(gb).Build());   // Built: no holders

// -- policies: independent modules register independent concerns ------------
graph->AddTuningPolicy(MemoryBudgetPolicy());     // sizes Full-edge holders to RAM/4
graph->AddTuningPolicy(PartitionBoundsPolicy());  // clamps max_partitions to cores

// -- stats time: footers read, hardware probed ------------------------------
QueryStats stats = CollectParquetStats(orders_ds, customers_ds);
HardwareInfo hw  = ProbeHardware();
ASSERT_OK_AND_ASSIGN(auto query, graph->Prepare(stats, hw));  // PreparedQuery

// -- run time: adapt if reality disagrees with the estimates ----------------
auto watchdog = std::jthread([&](std::stop_token st) {
  while (!st.stop_requested()) {
    if (memory_manager->should_spill())
      (void)query.ScaleFlowControl(0.5);     // stopgap; retired by phase 5's
    std::this_thread::sleep_for(100ms);      // downgrade executor
  }
});
ASSERT_OK(query.Execute());

// -- tomorrow: same shape, bigger file, fresh tuning -------------------------
ASSERT_OK_AND_ASSIGN(auto rerun, graph->Prepare(new_stats, hw));
```

---

## 9. Decisions (formerly open questions)

1. **`Chain()` refinement — accepted.** `Chain(...)` returns a `ChainBuilder` whose
   EdgeBuilder vocabulary applies to the chain's **last edge only** (§3.2). Rationale:
   covers the dominant "chain ends at a collector/blocking consumer" case in one
   expression; the last-edge rule is a single teachable sentence; mid-chain custom
   edges keep using `Edge()`.
2. **`PreparedQuery` — accepted.** `Prepare()` returns a new object owning holders,
   pipelines, and the executor (§2). States become types: structural tuning is
   compile-time restricted to `Graph`, soft tuning to `PreparedQuery`; a `Graph` is
   reusable across runs with fresh stats.
3. **Policy chain — decided by the phase review.** `AddTuningPolicy` (ordered chain),
   because phases 3/5/6 introduce at least three independently-authored tuning
   concerns (admission sizing, spill priorities, partition bounds) that must compose
   without one module owning the union (§6.4).
4. **Bulk live tuning — ship `ScaleFlowControl(factor)` as documented stopgap.**
   Useful to adaptive controllers now; explicitly retired in favor of phase 5's
   downgrade executor, which relieves pressure by moving *data* down tiers instead of
   throttling *admission* everywhere (§6.5).

---

## 10. Phase readiness matrix

What replication phases 3, 5, and 6 need from this design, and where it's provided:

| Phase | What it changes | What this design already provides | What was changed to be ready |
|---|---|---|---|
| **3 — task-creator hint chain** (data-driven tasks; no worker ever blocks in `Pull()`; backpressure = task admission) | Flow control stops meaning "block a thread" and starts meaning "don't mint a task"; pipelines get concurrency knobs | `FlowControl` defined as an admission condition (§3.3), so the DSL surface survives the engine swap; `PreparedQuery` isolates the executor swap from the API | **Wake contract** (§6.5): live threshold changes must post a task-re-evaluation event, not just a CV notify — added as a hard requirement (impl note §7.2). New surface: `Pipelines()` introspection + `SetMaxInFlightTasks()` reserved on `PreparedQuery` |
| **5 — predicate-driven spilling** (DataBlock idle⇄locked states; downgrade executor replaces eager `MaybeSpill`) | Spilling stops happening at `holder->Add` and becomes demand-driven victim selection | Edge-level spill knob already existed | `NoSpill()` generalized to `Spill(SpillPolicy{Eligible, Never, First})` with **phase-stable semantics**: today it maps to `auto_spill`; after phase 5 it feeds the downgrade executor's candidate enumeration (§3.3, §7.2). `ScaleFlowControl` explicitly documented as the stopgap the downgrade executor retires (§6.5) |
| **6 — blocking operators** (Limit → hash Aggregate → sample Sort → partitioned hash Join; converter-style synthesis of partition/merge stages and relay pipelines; PARTIAL barrier) | Edges appear that the author never wrote; partition counts are decided at RUNTIME from real input sizes (Sirius `determine_num_partitions`); a third barrier type | Port contracts + typed ports + shapes (§4) are exactly the extension recipe these operators use | Three additions: **`.Partial()` reserved** in the vocabulary (Build() rejects until the barrier exists, §3.3); **late-bound structural class** with `PartitionedUpTo(max)` — the resolution chain fixes bounds, the operator binds the value mid-execution (§2, §5); **synthesis-before-resolution ordering** + generated stable names for synthesized edges, so the policy chain and introspection see the planner's edges exactly like the author's (§5, §6.1, §7.3) |

The through-line: the DSL never encodes *how* the engine enforces a knob — only what
the knob means. That's what lets phases 3 and 5 swap enforcement mechanisms (thread
blocking → task admission; eager spill → demand-driven downgrade) under a stable API.
