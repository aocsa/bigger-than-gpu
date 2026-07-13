# P3: Incomplete Implementations

Features that are partially built and need completion.

## Tasks

### 3.1 `ConcatenatingBatchHolder::ConcatenateAll()` — multi-block case

- **File**: `src/quest/cache/batch_holder.cpp:214`
- **Status**: Returns error `"Concatenation of multiple RecordBatch blocks not yet implemented"`. Only single-block works.
- **Impact**: `BatchHolderType::kConcatenating` is non-functional for its primary use case.
- [ ] Implement multi-block concatenation (merge RecordBatches by appending column data)
- [ ] Write test with 3+ blocks being concatenated

### 3.2 `DataBlock::ToCpu()` / `ToDisk()` self-conversion

- **Files**: `src/quest/cache/data_block.hpp`, `data_block.cpp`
- **Status**: `HostMemoryBlock::ToCpu()` and `DiskBlock::ToDisk()` return errors because the object can't produce its own `shared_ptr`.
- **Fix**: Use `std::enable_shared_from_this<DataBlock>` or accept self-conversion as a no-op returning a wrapped copy.
- [ ] Implement self-conversion
- [ ] Test round-trip: CPU->Disk->CPU

### 3.3 `OptimizeScanWithLimit()` hardcoded value

- **File**: `src/quest/taskflow/graph.cpp:12,506`
- **Status**: Uses `kDefaultOptimizedLimitRows = 1000` instead of the actual limit from the downstream Limit operator.
- **Fix**: Read limit value from the Limit operator node.
- [ ] Parse actual limit from operator
- [ ] Remove hardcoded constant

### 3.4 `ExecuteStream()` not actually streaming

- **File**: `src/quest/builder/streaming_query_builder.hpp:130-155`
- **Status**: Collects ALL batches into a `std::vector` (line 138-145) before yielding — defeats backpressure.
- **Fix**: Requires async execution model or a separate producer thread feeding the coroutine.
- [ ] Implement true streaming (producer thread + coroutine bridge)
- [ ] Verify backpressure works end-to-end
