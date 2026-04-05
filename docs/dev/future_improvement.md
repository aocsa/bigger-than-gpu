# Future Improvements

## Current Project Status

### What Works Today
- **Parquet Reader**: Full read path for flat schemas — file reader, page reader, column reader with dictionary/plain encoding, Snappy + uncompressed codecs
- **Dataflow DSL**: Declarative graph construction with `>>` chaining, named ports (`op["port"]`), and `link()` with `EdgeConfig`
- **TaskFlow Engine**: BFS-scheduled parallel execution with one thread per operator, flow control via saturation thresholds
- **Cache Layer**: `BatchHolder` (FIFO) and `ConcatenatingBatchHolder` with `BlockingQueue`, backpressure, and auto-spill to disk
- **Memory Management**: Two-tier CPU/Disk with `MemoryResourceManager`, saturation tracking, and spill recommendations
- **Query Builder**: Split into `QueryBuilder` (sync: Execute, ExecuteCollect, ExecuteWithControl) and `StreamingQueryBuilder` (coroutine: ExecuteStream), sharing a `QueryPlan` fluent API
- **Logical Plan**: `Scan`, `Selection`, `Projection` nodes with expression trees (comparisons, arithmetic, aggregates)
- **Physical Plan**: `ScanExec`, `SelectionExec`, `ProjectionExec` with row-by-row evaluation
- **RecordBatch Serializer**: Binary format ("QRBT") for efficient batch I/O to disk
- **Smart Pointer Hygiene**: Zero raw `new`/`delete`, correct `shared_ptr`/`unique_ptr` split throughout

### Architecture Health
| Metric | Status |
|--------|--------|
| Build | Success |
| Tests | 473 passed, 1 skipped, 0 failed |
| Raw new/delete | Zero |
| Ownership issues | None detected |

---

## Improvements for Next Commit

### Priority 1 — Correctness Bugs

#### 1.1 Thread Safety in `Graph::Execute()`
- **File**: `src/quest/taskflow/graph.cpp`
- **Issue**: `processed_edges` is read inside `jthread` lambdas and written in the main loop without synchronization. This is a data race.
- **Fix**: Protect `processed_edges` with the existing `queue_mutex` or use a separate mutex.

#### 1.2 Busy-Wait Spin in Scheduler
- **File**: `src/quest/taskflow/graph.cpp`
- **Issue**: When operator dependencies aren't met, the scheduler re-pushes the operator and loops with no sleep or condition variable — 100% CPU burn.
- **Fix**: Add a condition variable that operator completion threads notify, so the scheduler blocks instead of spinning.

#### 1.3 `SimpleOperator::run()` Uses Wrong Error Code for End-of-Stream
- **File**: `src/quest/taskflow/operator.cpp`
- **Issue**: Uses `ErrorCode::Invalid` to detect end-of-stream. `Invalid` also means malformed input — these should not be conflated.
- **Fix**: Use `ErrorCode::EndOfData` consistently for normal stream termination.

#### 1.4 `MaterializedColumnVector::IsNull()` Always Returns False
- **File**: `src/quest/physical_plan/expressions/physical_expr.hpp`
- **Issue**: Null values from input columns are silently lost after filter/projection through materialized vectors.
- **Fix**: Add null bitmap to `MaterializedColumnVector`.

#### 1.5 Data Race on Saturation Threshold
- **File**: `src/quest/memory/memory_resource.hpp`
- **Issue**: `set_saturation_threshold()` writes under mutex, but `is_saturated()` reads without it.
- **Fix**: Make `saturation_threshold` atomic, or acquire the mutex on the read path.

---

### Priority 2 — Error Handling Consistency

#### 2.1 Replace Exceptions with `Result<>`
Several components throw exceptions instead of using the project's `Result<>` / `Status` convention:
| Component | File | Throws |
|-----------|------|--------|
| `RecordBatchSerializer` | `datasource/record_batch_serializer.hpp` | `std::runtime_error` |
| `MemoryResourceManager` ctor | `memory/memory_resource.hpp` | `std::invalid_argument` |
| `Port::get()` | `taskflow/port.hpp` | `std::runtime_error` |
| `Graph::GetLastOperator()` | `taskflow/graph.hpp` | `std::runtime_error` |

#### 2.2 Differentiate Queue States in `BlockingQueue`
- **File**: `src/quest/cache/blocking_queue.hpp`
- **Issue**: `Pop()` and `TryPop()` return `Status::Invalid` for both "queue empty" and "queue finished" — two semantically different states.
- **Fix**: Use `EndOfData` for finished+empty, `NotFound` for temporarily empty.

---

### Priority 3 — Incomplete Implementations

#### 3.1 `ConcatenatingBatchHolder::ConcatenateAll()` — Multi-Block Case
- **File**: `src/quest/cache/batch_holder.cpp`
- **Issue**: Returns error `"Concatenation of multiple RecordBatch blocks not yet implemented"`. Only single-block concatenation works.
- **Impact**: `BatchHolderType::kConcatenating` is non-functional for its primary use case.

#### 3.2 `DataBlock::ToCpu()` / `ToDisk()` Self-Conversion
- **Files**: `src/quest/cache/data_block.hpp`, `data_block.cpp`
- **Issue**: `HostMemoryBlock::ToCpu()` and `DiskBlock::ToDisk()` return errors because the object can't produce its own `shared_ptr`.
- **Fix**: Use `std::enable_shared_from_this<DataBlock>`.

#### 3.3 `OptimizeScanWithLimit()` Hardcoded Value
- **File**: `src/quest/taskflow/graph.cpp`
- **Issue**: Uses `kDefaultOptimizedLimitRows = 1000` instead of the actual limit from the `Limit` operator.
- **Fix**: Read limit from the downstream `Limit` operator node.

#### 3.4 `ExecuteStream()` Not Actually Streaming
- **File**: `src/quest/builder/query_builder.hpp`
- **Issue**: Collects ALL batches into memory before yielding — defeats backpressure.
- **Fix**: Requires async execution model or a separate producer thread feeding the coroutine.

---

### Priority 4 — Missing Plan Nodes

The `OperatorType` enum defines 19 operator types, but only 3 have logical + physical plan implementations:

| Plan Node | Logical | Physical | Operator |
|-----------|---------|----------|----------|
| Scan | `Scan` | `ScanExec` | `TableScan` |
| Filter | `Selection` | `SelectionExec` | `Filter` |
| Project | `Projection` | `ProjectionExec` | `Project` |
| Aggregate | — | — | `ComputeAggregate` |
| Sort | — | — | — |
| Limit | — | — | `Limit` |
| Join | — | — | `PartwiseJoin` / `JoinPartition` |
| Union | — | — | `Union` |

Next to implement: **Limit** (simplest) → **Aggregate** (expressions already exist) → **Sort** → **Join**.

---

### Priority 5 — Parquet Reader Gaps

#### 5.1 Only Flat Schemas Supported
- **Files**: `datasource/parquet/metadata.hpp`, `datasource/column_reader.hpp`
- **Issue**: `BuildSchema()` skips nested group elements. `ReadDataPage()` hard-codes `max_def_level = 0|1`. Nested Parquet (lists, maps, structs) is unsupported.

#### 5.2 Missing Compression Codecs
- **File**: `datasource/parquet/compression.hpp`
- **Issue**: Only Snappy and Uncompressed work. GZIP, LZO, Brotli, LZ4, ZSTD, LZ4_RAW all return `UnsupportedEncoding`.
- **Priority**: ZSTD and GZIP are the most commonly used in production.

#### 5.3 Logical Type Annotations Not Parsed
- **File**: `datasource/parquet/metadata.hpp`
- **Issue**: `ParseSchemaElement` skips `converted_type` (field ID 6). Date, timestamp, decimal annotations are never read.

#### 5.4 Dictionary Decoded on Every Page
- **File**: `datasource/column_reader.hpp`
- **Issue**: `ReadDictionaryPage()` stores raw bytes and re-decodes them on every `DecodeDictionaryValues` call.
- **Fix**: Decode the dictionary once and cache the decoded values.

---

### Priority 6 — Performance

#### 6.1 Row-by-Row Expression Evaluation
- **File**: `src/quest/physical_plan/expressions/physical_expr.hpp`
- **Issue**: All physical expressions evaluate via `GetValue(i)` through `std::variant` dispatch per row. No vectorized/columnar fast path.
- **Impact**: Orders of magnitude slower than columnar evaluation for large batches.

#### 6.2 Arithmetic Precision Loss
- **File**: `src/quest/physical_plan/expressions/physical_expr.hpp`
- **Issue**: All arithmetic upcasts to `double` — `int64_t` operations lose precision beyond 2^53.
- **Fix**: Type-specialized arithmetic that preserves integer semantics.

#### 6.3 No Type Promotion in Logical Expressions
- **File**: `src/quest/logical_plan/logical_expr.hpp`
- **Issue**: `ArithmeticBinaryExpr::ToField()` uses the left operand's type. `int32 + float` produces `int32`.

#### 6.4 Linear Scans in Schema and Queue
- `Schema::GetFieldByName()` / `GetFieldIndex()` — O(n) per lookup
- `BlockingQueue::PopById()` — O(n) per call

---

### Priority 7 — Platform & Robustness

#### 7.1 macOS Memory Detection
- **File**: `src/quest/memory/memory_resource.cpp`
- **Issue**: Falls back to hardcoded 8 GB RAM / 100 GB disk on non-Linux (including macOS where development happens).
- **Fix**: Use `sysctl(HW_MEMSIZE)` and `statvfs` on macOS.

#### 7.2 Serializer Endianness
- **File**: `src/quest/datasource/record_batch_serializer.hpp`
- **Issue**: Assumes little-endian via raw `memcpy`. Will produce corrupt data on big-endian platforms.

#### 7.3 Unsupported Types in Serializer
- **File**: `src/quest/datasource/record_batch_serializer.hpp`
- **Issue**: `Int96Type`, `Int8Type`, `Int16Type`, `FixedLenByteArrayType` all throw on serialization.

---

## Long-Term Roadmap

These are larger efforts beyond the next few commits:

1. **Vectorized Expression Engine** — columnar evaluation with SIMD where applicable
2. **Query Optimizer** — predicate push-down, projection push-down, join reordering
3. **Aggregate / GroupBy Pipeline** — hash aggregation with spill support
4. **Sort-Merge Join** — partition-aware join with spill to disk
5. **GPU Tier** — CUDA memory resource and GPU-resident `DataBlock`
6. **Distributed Execution** — network tier with `NetworkDataBlock`
7. **Compression Before Spill** — compress data blocks before writing to disk
8. **Memory Pools** — arena-based allocation for batch processing
9. **Checkpointing** — save/restore execution state for fault tolerance
10. **Observability** — OpenTelemetry metrics for operator throughput, memory pressure, spill rates
