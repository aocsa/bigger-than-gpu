# How Sirius `create_plan()` Works (Phase 1 note)

Sirius does not maintain a capability table. Support **is** whatever
`sirius_physical_plan_generator::create_plan()` can translate.

## The gate

1. DuckDB hands Sirius a `LogicalOperator` tree (post-optimizer hook stores a copy).
2. `create_plan(unique_ptr<LogicalOperator>)` resolves types + column bindings, then
   dispatches `create_plan(LogicalOperator&)` — a big `switch` on operator type
   (`sirius_physical_plan_generator.cpp` ~112+).
3. Each supported type has an overload (`create_plan(LogicalFilter&)`,
   `create_plan(LogicalGet&)`, …). Unsupported types
   `throw NotImplementedException("… not supported")`.
4. On success the GPU physical tree feeds pipelines / task creator / scheduler.
5. On throw (or later GPU execute failure), the extension catches and runs
   `run_internal_cpu_fallback_query` — the **untouched DuckDB CPU plan** is the
   executor of last resort (`sirius_extension.cpp`). Local reads can fall back;
   S3 has no CPU path and surfaces a clear error instead.

## What to steal for QUEST

| Sirius | QUEST Phase 1 |
|---|---|
| One function translates the whole plan or fails | `CreateGraph(LogicalPlan&, MemoryResourceManagerPtr) -> Result<GraphPtr>` |
| Throw = unsupported | `Result` error (no exceptions) |
| CPU plan = last resort | Existing callback `CreatePhysicalPlan` + `PhysicalPlan::Execute` |
| Support = what the gate translates | Only Scan / Selection / Projection (+ collector sink) |
| Physical ops feed the pipeline runtime | Role-based taskflow ops → existing `Graph::Execute()` → `PipelineExecutor` |

## What not to copy

GPU streams, device colocation, DuckDB extension hooks, S3-specific fallback rules.
The learning target is the **single gate + fallback discipline**, then feeding real
operators into the push pipeline chassis Phase 2 already built.
