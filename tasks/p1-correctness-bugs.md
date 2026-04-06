# P1: Correctness Bugs

Small, focused fixes. Each one is a single commit.

## Tasks

### 1.1 Data race in `Graph::Execute()` — `processed_edges`

- **File**: `src/quest/taskflow/graph.cpp:186`
- **Bug**: `processed_edges` (std::set) is read inside jthread lambdas (line 223-225) and written in the main loop (line 246) without synchronization.
- **Fix**: Protect `processed_edges` with the existing `queue_mutex` or a separate mutex.
- [ ] Implement fix
- [ ] Write test proving race is fixed (use ThreadSanitizer or concurrent stress test)

### 1.2 Busy-wait spin in scheduler

- **File**: `src/quest/taskflow/graph.cpp`
- **Bug**: When operator dependencies aren't met, the scheduler re-pushes the operator and loops with no sleep or condition variable — 100% CPU burn.
- **Fix**: Add a condition variable that operator completion threads notify, so the scheduler blocks instead of spinning.
- [ ] Implement fix
- [ ] Verify CPU usage drops to near-zero when waiting

### 1.3 `SimpleOperator::run()` uses wrong error code for end-of-stream

- **File**: `src/quest/taskflow/operator.cpp:105`
- **Bug**: Uses `ErrorCode::Invalid` to detect end-of-stream. `Invalid` also means malformed input — conflated semantics.
- **Note**: `ErrorCode::EndOfData` already exists in `result.hpp:311`.
- **Fix**: Change the check from `ErrorCode::Invalid` to `ErrorCode::EndOfData` in `operator.cpp`, and update `BlockingQueue` to return `EndOfData` when finished+empty.
- [ ] Update `BlockingQueue::Pop()`/`TryPop()` to return `EndOfData` for finished+empty
- [ ] Update `SimpleOperator::run()` to check `EndOfData`
- [ ] Verify existing tests still pass

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
