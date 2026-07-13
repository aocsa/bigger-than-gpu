# Future Improvements

Big refactors and architectural improvements. For small bugs and focused tasks, see `tasks/`.

## Current Project Status

### What Works Today
- **Parquet Reader**: Flat schemas — file reader, page reader, column reader with dictionary/plain encoding, Snappy + uncompressed codecs
- **Dataflow DSL**: Declarative graph construction with `>>` chaining, named ports, `link()` with `EdgeConfig`
- **TaskFlow Engine**: BFS-scheduled parallel execution with one thread per operator, flow control via saturation thresholds
- **Cache Layer**: `BatchHolder` (FIFO) and `ConcatenatingBatchHolder` with `BlockingQueue`, backpressure, and auto-spill to disk
- **Memory Management**: Two-tier CPU/Disk with `MemoryResourceManager`, saturation tracking, and spill recommendations
- **Query Builder**: `QueryBuilder` (sync) and `StreamingQueryBuilder` (coroutine), sharing `QueryPlan` fluent API
- **Logical Plan**: `Scan`, `Selection`, `Projection` nodes with expression trees
- **Physical Plan**: `ScanExec`, `SelectionExec`, `ProjectionExec` with row-by-row evaluation
- **RecordBatch Serializer**: Binary format ("QRBT") for efficient batch I/O to disk

### Architecture Health
| Metric | Status |
|--------|--------|
| Build | Success |
| Tests | 473 passed, 1 skipped, 0 failed |
| Raw new/delete | Zero |
| Ownership issues | None detected |

---

## Missing Plan Nodes

The `OperatorType` enum defines 19 types, but only 3 have logical + physical plan implementations:

| Plan Node | Logical | Physical | Operator | Priority |
|-----------|---------|----------|----------|----------|
| Scan | `Scan` | `ScanExec` | `TableScan` | Done |
| Filter | `Selection` | `SelectionExec` | `Filter` | Done |
| Project | `Projection` | `ProjectionExec` | `Project` | Done |
| Limit | — | — | `Limit` | Next |
| Aggregate | — | — | `ComputeAggregate` | After Limit |
| Sort | — | — | — | After Aggregate |
| Join | — | — | `PartwiseJoin` / `JoinPartition` | After Sort |
| Union | — | — | `Union` | Low |

Use `/new-node` command to scaffold new plan nodes.

---

## Parquet Reader Gaps

### Nested Schema Support
- **Files**: `datasource/parquet/metadata.hpp`, `datasource/column_reader.hpp`
- `BuildSchema()` skips nested group elements. `ReadDataPage()` hard-codes `max_def_level = 0|1`. Nested Parquet (lists, maps, structs) is unsupported.

### Missing Compression Codecs
- **File**: `datasource/parquet/compression.hpp`
- Only Snappy and Uncompressed work. GZIP, LZO, Brotli, LZ4, ZSTD, LZ4_RAW all return `UnsupportedEncoding`.
- Priority: ZSTD and GZIP are the most commonly used in production.

### Logical Type Annotations
- **File**: `datasource/parquet/metadata.hpp`
- `ParseSchemaElement` stores `logical_type` but doesn't parse `converted_type` (field ID 6). Date, timestamp, decimal annotations are never read.

### Dictionary Re-Decoding
- **File**: `datasource/column_reader.hpp`
- `DecodeDictionaryValues()` re-decodes the dictionary from raw bytes on every call. Should decode once and cache.

---

## Performance

### Vectorized Expression Engine
- **File**: `src/quest/physical_plan/expressions/physical_expr.hpp`
- All physical expressions evaluate via `GetValue(i)` through `std::variant` dispatch per row. No vectorized/columnar fast path. Orders of magnitude slower than columnar evaluation for large batches.

### Arithmetic Precision
- **File**: `src/quest/physical_plan/expressions/physical_expr.hpp`
- All arithmetic upcasts to `double` — `int64_t` operations lose precision beyond 2^53. Need type-specialized arithmetic.

### Type Promotion
- **File**: `src/quest/logical_plan/logical_expr.hpp:272`
- `ArithmeticBinaryExpr::ToField()` uses the left operand's type. `int32 + float` produces `int32`.

### Linear Scans
- `Schema::GetFieldByName()` / `GetFieldIndex()` — O(n) per lookup. Use hash map for large schemas.
- `BlockingQueue::PopById()` — O(n) per call.

---

## Platform & Robustness

### macOS Memory Detection
- **File**: `src/quest/memory/memory_resource.cpp`
- Falls back to hardcoded 8 GB RAM / 100 GB disk on non-Linux. Use `sysctl(HW_MEMSIZE)` and `statvfs` on macOS.

### Serializer Endianness
- **File**: `src/quest/datasource/record_batch_serializer.hpp`
- Assumes little-endian via raw `memcpy`. Will produce corrupt data on big-endian platforms.

---

## Long-Term Roadmap

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

---

## Task Files (moved from this doc)

| Task File | What moved there |
|-----------|-----------------|
| `tasks/p1-correctness-bugs.md` | P1 bugs: data races, wrong error codes, null bitmap |
| `tasks/p2-error-handling.md` | P2: exception-to-Result migration, queue state differentiation |
| `tasks/p3-incomplete-implementations.md` | P3: ConcatenateAll, ToCpu/ToDisk, hardcoded limit, fake streaming |
