---
name: review-memory-safety
description: Reviews code changes against QUEST's ownership model. Checks pointer misuse, data races, and memory leaks. Trigger: "review memory", "check ownership", "review safety", "memory review"
---

# Review Memory Safety

Reviews code changes against QUEST's ownership model and known concurrency issues.

## Read These Files First

- The files being reviewed (diff or changed files)
- `tasks/p1-correctness-bugs.md` — known data races and bugs

## Ownership Model

| Pointer Type | Used For | Why |
|-------------|----------|-----|
| `unique_ptr` | Expression trees (`LogicalExprPtr`, physical `ExpressionPtr`), plan nodes (`LogicalPlanPtr`), operators built by `CreateGraph`, compression codecs, column vectors from `CreateColumnVector` | Single owner, no sharing needed |
| `shared_ptr` | `IBatchHolder` (shared between source/target operators), `MemoryResourceManager` (one per graph), `DataSource` (shared across plan layers), `ColumnVector` in `RecordBatch` (zero-copy slicing), `DataBlock` (multi-consumer cache) | Multiple owners or cross-boundary sharing |
| `weak_ptr` | `Operator::owning_graph_` (back-reference to `Graph`) | Break circular dependency: Graph owns Operators, Operators reference Graph |

## Checklist

Review each changed file against these rules:

### FAIL (must fix)

1. **Raw `new`/`delete`** — Any raw allocation. Use `make_unique`/`make_shared` exclusively.
2. **Missing `.lock()` on `weak_ptr`** — `owning_graph_` must be locked before use. Check for null after lock.
3. **`shared_ptr` cycles** — A->B->A via `shared_ptr`. Look for operators holding `shared_ptr<Graph>` instead of `weak_ptr`.
4. **`BlockingQueue` accessed without mutex** — All queue operations must go through the thread-safe interface.
5. **Dangling pointer** — Raw pointer to a moved-from `unique_ptr`, or reference to destroyed temporary.

### WARNING (should fix)

6. **`shared_ptr` where `unique_ptr` suffices** — If only one owner exists, prefer `unique_ptr`.
7. **`DataBlock` not released after spill-to-disk** — When data is spilled from CPU to disk, the CPU `DataBlock` should be released.
8. **Atomic access without proper memory ordering** — Check `std::memory_order` on atomic operations. Default (`seq_cst`) is safe but may be wrong intent.
9. **Missing null check after `weak_ptr::lock()`** — The graph could be destroyed.

### INFO (note for awareness)

10. **Known data race: `saturation_threshold`** — P1.5 in `tasks/p1-correctness-bugs.md`. Written under mutex, read without it in `is_saturated()`.

## Specific Patterns to Watch

### BatchHolder Sharing

BatchHolders are `shared_ptr<IBatchHolder>` because they connect two operators:
- Source operator's output port holds a reference
- Target operator's input port holds a reference
- Producer and consumer pipelines may run concurrently on different `PipelineExecutor` workers

Verify: thread-safe access only through `Add()`, `Pull()`, `Finish()`, `WaitIfSaturated()`.

### DataBlock Lifecycle

```
HostMemoryBlock (CPU) --spill--> DiskBlock (Disk) --load--> HostMemoryBlock (CPU)
```

After spill: the original CPU block must be `Release()`d to free memory.
After load: the disk file may or may not be cleaned up (depends on `DiskMemoryConfig::cleanup_on_destroy`).

### Expression Tree Ownership

Expression trees are always `unique_ptr`:
- `LogicalExprPtr = unique_ptr<LogicalExpr>`
- `ExpressionPtr = unique_ptr<Expression>` (physical)

NEVER shared. Each plan node owns its expressions exclusively. Moving expressions transfers ownership.

### Plan Node Ownership

```
QueryPlan owns LogicalPlanPtr (unique_ptr<LogicalPlan>)
  -> Selection owns LogicalPlanPtr (its input) + LogicalExprPtr (condition)
    -> Scan owns shared_ptr<DataSource>
```

Lowering mirrors this: `CreateGraph` returns an `ExecutableGraph` that owns the operators as
`unique_ptr<Operator>`; the `Graph` and its edges hold non-owning `Operator*`.

## Verdict Format

```
## Memory Safety Review: {files reviewed}

| # | Rule | Status | Location | Details |
|---|------|--------|----------|---------|
| 1 | No raw new/delete | PASS/FAIL | file:line | ... |
| 2 | weak_ptr locked | PASS/FAIL | file:line | ... |
| ... | ... | ... | ... | ... |

Overall: PASS / WARNING / FAIL
```

- **PASS**: No FAIL items, warnings are acceptable
- **WARNING**: No FAIL items, but warnings present that should be addressed
- **FAIL**: At least one FAIL item found
