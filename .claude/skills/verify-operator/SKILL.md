---
name: verify-operator
description: Adversarial testing that tries to BREAK a QUEST operator across 6 failure dimensions. Trigger: "verify operator", "test operator", "break this operator", "stress test"
---

# Verify Operator — Adversarial Tester

Systematically attempts to break an operator by testing the 6 failure dimensions specific to QUEST's architecture.

## Read These Files First

- The operator under test (its `.hpp` and `.cpp`)
- `src/quest/cache/batch_holder.hpp` — `IBatchHolder` interface: `Add()`, `Pull()`, `Finish()`
- `src/quest/cache/data_block.hpp` — `HostMemoryBlock`, `DiskBlock`
- `src/quest/cache/blocking_queue.hpp` — thread safety via mutex + condition_variable
- `src/quest/memory/memory_resource.hpp` — 85% CPU threshold, `is_saturated()`, `should_spill()`
- `docs/dev/future_improvement.md` — known bugs that may affect test results

## 6-Point Test Strategy

### 1. Empty/Null DataBlocks

- Pass a `DataBlock` with 0 rows through the operator
- Pass a `DataBlock` where all column values are null
- Pass a `nullptr` DataBlock (if interface allows)
- Expected: operator handles gracefully, no crash, no UB

### 2. Backpressure: Downstream Full

- Set `BatchHolderConfig::flow_control_batches_threshold` to 1
- Push multiple batches into the operator's output BatchHolder
- Verify the operator respects `WaitIfSaturated()` and doesn't overflow
- Check that `can_you_throttle_my_input()` returns the correct value

### 3. Spill Boundary: 84% -> 86% Memory

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

### 5. Concurrent Access

- Two operators sharing the same `IBatchHolder`
- One thread calling `Add()`, another calling `Pull()` simultaneously
- Verify `BlockingQueue` mutex protects against data races
- Known bug: `processed_edges` race in `Graph::Execute()` (P1.1)

### 6. Throttle Compliance

- Verify `can_you_throttle_my_input()` returns the expected value
- If `true`: verify the operator actually pauses when downstream is saturated
- If `false`: verify the operator documents WHY it can't throttle
- For `SimpleOperator` subclasses: throttle is always `true` (base class default)

## Execution

Use a subagent to run the tests. For each dimension:

1. Read the operator source to understand its specific behavior
2. Write targeted test cases
3. Run the tests
4. Report results

## Verdict Format

For each of the 6 dimensions, report:

```
## Verdict: {OPERATOR_NAME}

| Dimension | Status | Details |
|-----------|--------|---------|
| Empty/Null | PASS/FAIL | file:line reference |
| Backpressure | PASS/FAIL | file:line reference |
| Spill Boundary | PASS/PARTIAL | "blocked by known P1.5 bug" |
| Type Edge Cases | PASS/FAIL | file:line reference |
| Concurrent Access | PASS/FAIL | file:line reference |
| Throttle Compliance | PASS/FAIL | file:line reference |

Overall: PASS / PARTIAL / FAIL
```

- **PASS**: All 6 dimensions pass
- **PARTIAL**: Some dimensions blocked by known bugs (reference `future_improvement.md`)
- **FAIL**: At least one dimension fails with a new (not previously known) bug

## Gotchas

- `MaterializedColumnVector::IsNull()` always returns false (P1.4) — affects null testing
- `SimpleOperator::run()` uses `ErrorCode::Invalid` for end-of-stream (P1.3) — don't confuse with actual errors
- `ConcatenatingBatchHolder` multi-block case is not implemented (P3.1) — use `BatchHolder` (kSimple) for testing
- `DataBlock::ToCpu()` on a `HostMemoryBlock` returns error (P3.2) — can't round-trip self-conversion
