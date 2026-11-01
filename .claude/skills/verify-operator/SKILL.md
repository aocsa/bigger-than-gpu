---
name: verify-operator
description: Adversarial testing that tries to BREAK a QUEST operator across 5 failure dimensions. Trigger: "verify operator", "test operator", "break this operator", "stress test"
---

# Verify Operator — Adversarial Tester

Systematically attempts to break an operator by testing the 5 failure dimensions specific to
QUEST's push pipeline architecture. The old thread-per-operator executor (`ExecuteLegacy`) is
deleted; all operators run through `Graph::Execute()` → `MetaPipeline::Build` →
`PipelineExecutor`. Tests must wire through `GraphBuilder`/`PreparedQuery`, not the legacy DSL.

## Read These Files First

- The operator under test (`src/quest/taskflow/operators/{name}_operator.hpp`)
- `src/quest/cache/batch_holder.hpp` — `IBatchHolder` interface: `Add()`, `Pull()`, `Finish()`
- `src/quest/cache/data_block.hpp` — `HostMemoryBlock`, `DiskBlock`
- `src/quest/cache/blocking_queue.hpp` — thread safety via mutex + condition_variable
- `src/quest/memory/memory_resource.hpp` — 85% CPU threshold, `is_saturated()`, `should_spill()`
- `src/quest/taskflow/pipeline_executor.hpp` — claim/task-creation discipline
- `tasks/p1-correctness-bugs.md`, `tasks/p3-incomplete-implementations.md` — known bugs that may affect test results

## 5-Point Test Strategy

### 1. Empty/Null DataBlocks

- Pass a `DataBlock` with 0 rows through the operator (`ProcessBatch` / `Sink`)
- Pass a `DataBlock` where all column values are null
- Pass a `nullptr` DataBlock (if interface allows)
- Expected: operator handles gracefully, no crash, no UB

### 2. Holder Boundaries and Backpressure

- Set `BatchHolderConfig::flow_control_batches_threshold` to 1
- Test `WaitIfSaturated()` directly with a producer and consumer; the current executor does not
  call it on behalf of an operator
- Verify a cross-pipeline consumer blocks in `Pull()` until input arrives or the holder finishes
- Verify intra-pipeline execution does not assume a holder exists between fused operators

### 3. Spill Boundary: 84% → 86% Memory

- Configure `CPUMemoryResource` with a small `max_bytes` and 85% threshold
- Fill memory to ~84% via `record_allocation()`
- Push a batch that crosses the 86% boundary
- Verify: BatchHolder triggers spill to disk (`DiskBlock` created)
- Known bug: `set_saturation_threshold()` has a data race (P1.5) — note in verdict

### 4. Type Edge Cases

- NULL columns (known bug: `IsNull()` always returns false — P1.4)
- Zero-length strings
- `INT64_MAX` / `INT32_MIN` values
- Empty string columns
- Single-row batches
- Very large batches (10K+ rows)

### 5. Concurrent Access via Graph::Execute

- Wire the operator in a full pipeline and call `Graph::Execute()` (which uses `PipelineExecutor`)
- Run with two concurrent drains sharing an inter-pipeline `BatchHolder`
  (one thread calling `Add()`, another calling `Pull()`)
- Verify `BlockingQueue` mutex protects against data races
- For blocking operators: verify `FinalizeSink()` is called exactly once and the
  `status_lock_` / `MarkTaskCreated` / `MarkTaskCompleted` claim sequence is correct

## Pipeline Role Checks

For each operator, verify the role contract before writing tests:

| Role | Method | Contract |
|------|--------|---------|
| Streaming | `ProcessBatch(batch)` | Returns transformed batch or error; never returns nullopt |
| Source | `IsSource()` → true, `GetNextBatch()` | Returns `Result<optional<DataBlockPtr>>`; `nullopt` = EOS (not an error) |
| Sink | `IsSink()` → true, `Sink(batch)` | Accumulates; returns success (`{}`) on each batch |
| Finalize | `FinalizeSink()` | Called once; flips operator to source mode |
| Blocking source | `IsSource()` → true, `GetNextBatch()` | Emits accumulated batches after finalize; nullopt = done |

## Execution

Use a subagent to run the tests. For each dimension:

1. Read the operator source to understand its specific behavior
2. Write targeted test cases using `GraphBuilder`/`PreparedQuery`
3. Run the tests
4. Report results

## Verdict Format

For each of the 5 dimensions, report:

```
## Verdict: {OPERATOR_NAME}

| Dimension | Status | Details |
|-----------|--------|---------|
| Empty/Null | PASS/FAIL | file:line reference |
| Holder Boundaries/Backpressure | PASS/FAIL | file:line reference |
| Spill Boundary | PASS/PARTIAL | "blocked by known P1.5 bug" |
| Type Edge Cases | PASS/FAIL | file:line reference |
| Concurrent Access (Graph::Execute) | PASS/FAIL | file:line reference |

Overall: PASS / PARTIAL / FAIL
```

- **PASS**: All 5 dimensions pass
- **PARTIAL**: Some dimensions blocked by known bugs (reference `tasks/p1-correctness-bugs.md` / `tasks/p3-incomplete-implementations.md`)
- **FAIL**: At least one dimension fails with a new (not previously known) bug

## Gotchas

- `MaterializedColumnVector::IsNull()` always returns false (P1.4) — affects null testing
- `DataBlock::ToCpu()` on a `HostMemoryBlock` returns error (P3.2) — can't round-trip self-conversion
- EOS from `GetNextBatch()` is an engaged `std::nullopt` — never an error; any error from a source is a real error
