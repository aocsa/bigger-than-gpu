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
- Which fluent methods are chained? (`Where`, `Select`)
- What expressions are used?

### Layer 2: Logical Plan

File: `src/quest/logical_plan/logical_plan.hpp`

- Which `LogicalPlan` nodes are created? (`Scan`, `Selection`, `Projection`)
- What's the tree structure? (use `Pretty()` output format)
- What's the schema at each node?

### Layer 3: Physical Plan

File: `src/quest/physical_plan/physical_plan.hpp`

- How does `CreatePhysicalPlan()` map each logical node?
  - `Scan` -> `ScanExec`
  - `Selection` -> `SelectionExec` (+ `CreatePhysicalExpr` for the condition)
  - `Projection` -> `ProjectionExec` (+ `CreatePhysicalExpr` for each expression)
- What physical expressions are created?

### Layer 4: TaskFlow Graph

File: `src/quest/taskflow/graph.hpp`

- What operators are wired? Which `OperatorType` for each?
- What edges connect them? (source_port -> target_port)
- Is `OptimizeScanWithLimit()` applicable?
- BFS scheduling order?

### Layer 5: Cache Layer

File: `src/quest/cache/batch_holder.hpp`

- Which `BatchHolderType` for each edge? (`kSimple` vs `kConcatenating`)
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
    .Where(col("age") > lit(25))
    .Select({"name", "age"})

### Layer 2: Logical Plan
  Projection: name, age
    Selection: age > 25
      Scan: data.parquet; projection=None

### Layer 3: Physical Plan
  ProjectionExec([ColumnExpr(0), ColumnExpr(1)])
    SelectionExec(GtExpr(ColumnExpr(1), LiteralExpr(25)))
      ScanExec([name, age])

### Layer 4: TaskFlow Graph
  [0] TableScan -> [1] Filter -> [2] Project -> [3] Output
  Edges: 0->1 (default), 1->2 (default), 2->3 (default)

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
- If the query uses plan nodes that don't exist yet (e.g., Aggregate), note that and reference `docs/dev/future_improvement.md` P4.
- Known: only 3 plan nodes are implemented (Scan, Selection, Projection). Trace as far as possible.
