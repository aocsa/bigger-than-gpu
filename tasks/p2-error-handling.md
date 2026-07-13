# P2: Error Handling Consistency

Replace exceptions with `Result<>` / `Status` across 4 components.

## Tasks

### 2.1 Replace exceptions with `Result<>`

These components throw exceptions instead of using the project's error convention:

- [ ] `RecordBatchSerializer` (`src/quest/datasource/record_batch_serializer.hpp`) — 9 `throw std::runtime_error` calls
- [ ] `MemoryResourceManager` ctor (`src/quest/memory/memory_resource.cpp:196-199`) — `throw std::invalid_argument`
- [ ] `Port::get()` (`src/quest/taskflow/port.cpp:130,140`) — `throw std::runtime_error`
- [ ] `Graph::GetLastOperator()` (`src/quest/taskflow/graph.cpp:294,301`) — `throw std::runtime_error`
- [ ] `Schema` methods (`src/quest/datasource/schema.hpp:72,99,114`) — `throw std::out_of_range/invalid_argument`
- [ ] `RecordBatch` ctor + column() (`src/quest/datasource/record_batch.hpp:23,39,46`) — `throw std::invalid_argument/out_of_range`
- [ ] `ColumnVector::GetValue()` (`src/quest/datasource/column_vector.hpp:85`) — `throw std::out_of_range`

### 2.2 Differentiate queue states in `BlockingQueue`

- **File**: `src/quest/cache/blocking_queue.hpp`
- **Bug**: `Pop()` and `TryPop()` return `Status::Invalid` for both "queue empty" and "queue finished" — two semantically different states.
- **Fix**: Use `EndOfData` for finished+empty, `NotFound` for temporarily empty.
- **Note**: Overlaps with P1.3 — fix together.
- [ ] Differentiate return codes
- [ ] Update all callers
