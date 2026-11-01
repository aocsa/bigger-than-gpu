---
name: trace-dataflow
description: Read-only trace of how a query flows through all 6 QUEST layers. Uses a subagent. Trigger: "trace", "follow the data", "how does this query execute", "debug flow"
---

# Trace Dataflow

Traces a query through all 6 QUEST layers, showing what happens at each stage. This is a READ-ONLY skill — it MUST NOT modify any file.

## Execution

Delegate the trace to a subagent with the query or code snippet to trace.

## The 6 Layers (in execution order)

### Layer 1: QueryPlan DSL

File: `src/quest/builder/query_plan.hpp`

- Which factory method starts the plan? (`FromParquet`, `From`)
- Which fluent methods are chained? (`Where`, `Select`, `Limit`, `UnionAll`)
- What expressions are used?

### Layer 2: Logical Plan

File: `src/quest/plan/logical/logical_plan.hpp`

- Which `LogicalPlan` nodes are created? (`Scan`, `Selection`, `Projection`, `Limit`, `Union`)
- What's the tree structure? (use `Pretty()` output format)
- What's the schema at each node?

### Layer 3: Lowering

File: `src/quest/execution/lowering/plan_to_graph.cpp` (`CreateGraph`, `TranslateNode`, `CreatePhysicalExpr`)

- How does `TranslateNode()` map each logical node to a taskflow operator?
  - `Scan` -> `TableScanOperator`
  - `Selection` -> `FilterOperator` (+ `CreatePhysicalExpr` for the condition)
  - `Projection` -> `ProjectOperator` (+ `CreatePhysicalExpr` for each expression), or folded
    into one `BindableTableScan` when it only selects columns directly over a `Scan`
  - `Limit` -> `LimitOperator`
  - `Union` -> `UnionOperator` (children wired to `input_a` / `input_b`)
  - `CreateGraph` appends a `CollectorOperator` after the root
- What physical expressions are created? (`src/quest/plan/physical/expressions/physical_expr.hpp`)

### Layer 4: TaskFlow Graph

Files: `src/quest/taskflow/graph.hpp`, `meta_pipeline.hpp`, `pipeline_executor.hpp`

- What operators are wired? Which `OperatorType` for each?
- What edges connect them? (source_port -> target_port; Streaming or Full barrier, inferred
  from the consumer's `InputEdgeContract()` by `GraphBuilder::Build()`)
- How does `MetaPipeline::Build()` split the graph into pipelines? (streaming operators fuse;
  blocking operators and Full edges break)
- In what order does `PipelineExecutor` run the pipelines?

### Layer 5: Cache Layer

File: `src/quest/cache/batch_holder.hpp`

- Which `BatchHolderType` for each edge? (`kSimple`, `kConcatenating`, `kNonWaiting`; holders are created in `Graph::Prepare()`)
- What `BatchHolderConfig` settings apply? (flow control thresholds, timeout, auto_spill)
- How does `BlockingQueue` coordinate producers and consumers?

### Layer 6: Memory Management

File: `src/quest/memory/memory_resource.hpp`

- Which `MemoryTier` will the data start in? (CPU)
- What's the saturation threshold? (default 85% for CPU, 90% for disk)
- Will `should_spill()` trigger? Estimate based on data size
- What's `recommend_tier()` going to return?

## Output Format

```
## Query Trace: {query description}

### Layer 1: QueryPlan DSL
  FromParquet("data.parquet")
    .Where(Col("age") > Lit(25))
    .Select({"name", "age"})

### Layer 2: Logical Plan
  Projection: #name, #age
    Selection: (#age > 25)
      Scan: data.parquet; projection=None

### Layer 3: Lowering
  Scan       -> [0] TableScanOperator (all columns)
  Selection  -> [1] FilterOperator(#1 > 25)
  Projection -> [2] ProjectOperator([#0, #1])  (not folded: input is a Selection)
  root       -> [3] CollectorOperator

### Layer 4: TaskFlow Graph
  [0] TableScan -> [1] Filter -> [2] Project -> [3] Output (Collector)
  Edges: 0->1 (Streaming), 1->2 (Streaming), 2->3 (Full, Collector contract)
  Pipelines: {MetaPipeline::Build result, source -> ... -> sink}

### Layer 5: Cache Layer
  Edge 0->1: BatchHolder(kSimple), auto_spill=true
  Edge 1->2: BatchHolder(kSimple), auto_spill=true
  Edge 2->3: BatchHolder(kSimple), auto_spill=true

### Layer 6: Memory Estimate
  Input: ~{N} MB (Parquet file size)
  CPU threshold: 85% of {max_bytes}
  Spill likely: {yes/no}
  Recommendation: {CPU/Disk}
```

## Important

- This skill is READ-ONLY. Do NOT modify any file.
- Use a subagent for the exploration to keep main context clean.
- If the query uses plan nodes that don't exist yet (e.g., Aggregate), note that; `CreateGraph` rejects them with `PhysicalPlanError`.
- Known: only 5 plan nodes are implemented (Scan, Selection, Projection, Limit, Union). Trace as far as possible.
