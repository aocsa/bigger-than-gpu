# P1: Correctness Bugs

Small, focused fixes. Each one is a single commit.

> **Note (2026-08-01):** The legacy thread-per-operator executor has been deleted. P1.1, P1.2,
> and P1.3 were specific to that scheduler (`Graph::Execute()`'s BFS loop,
> `SimpleOperator::run()`, and `BlockingQueue` EOS signaling). All three are now **obsolete**
> — the pipelined engine has no BFS loop, no `run()` method, and EOS is a value
> (`std::optional` nullopt), not an error code. P1.4 and P1.5 remain open.

## Tasks

### ~~1.1 Data race in `Graph::Execute()` — `processed_edges`~~ (OBSOLETE — executor deleted)

- ~~**File**: `src/quest/taskflow/graph.cpp:186`~~
- ~~**Bug**: `processed_edges` (std::set) is read inside jthread lambdas without synchronization.~~
- **Status**: The BFS scheduler and `processed_edges` were removed when the legacy executor was deleted.

### ~~1.2 Busy-wait spin in scheduler~~ (OBSOLETE — executor deleted)

- ~~**File**: `src/quest/taskflow/graph.cpp`~~
- ~~**Bug**: Scheduler re-pushes operators with no sleep or condition variable.~~
- **Status**: The BFS scheduler was removed when the legacy executor was deleted.

### ~~1.3 `SimpleOperator::run()` uses wrong error code for end-of-stream~~ (OBSOLETE — executor deleted)

- ~~**File**: `src/quest/taskflow/operator.cpp:105`~~
- ~~**Bug**: Uses `ErrorCode::Invalid` to detect end-of-stream.~~
- **Status**: `SimpleOperator::run()` and the legacy EOS-via-error-code path were deleted.
  The pipeline engine signals EOS as an engaged `std::nullopt` from `GetNextBatch()`.

### 1.4 `MaterializedColumnVector::IsNull()` always returns false

- **File**: `src/quest/physical_plan/expressions/physical_expr.hpp:63`
- **Bug**: `IsNull()` returns `false` unconditionally. Null values from input columns are lost after filter/projection.
- **Fix**: Add a null bitmap (`std::vector<bool>`) to `MaterializedColumnVector`. Populate it during construction in `SelectionExec::FilterColumn()`.
- [ ] Add null bitmap to `MaterializedColumnVector`
- [ ] Populate bitmap in `FilterColumn()`
- [ ] Write test with null values through filter+projection

### 1.5 Data race on saturation threshold

- **File**: `src/quest/memory/memory_resource.cpp:75,84`
- **Bug**: `set_saturation_threshold()` writes `config_.saturation_threshold` under mutex (line 84), but `is_saturated()` reads it without mutex (line 75).
- **Fix**: Make saturation_threshold atomic, or acquire mutex on read path. Atomic is simpler.
- [ ] Fix the race
- [ ] Write test with concurrent set/read of threshold
