# Slim-Down Plan — Brief

**Goal:** shrink `src/quest/` (14.9k LOC) to only what the taskflow engine needs, remove all
macros, and tighten C++23 quality. Every stage ends with `cmake --build build && ctest` green.

**Expected result:** ~2,100 LOC removed safely (~14%), ~2,650 if the open decisions below go
"delete". Roughly 1,000 LOC of tests/examples go with it.

**Engine path (what stays):** `QueryBuilder::ExecuteOnGraph` → `execution::CreateGraph` →
`taskflow::GraphBuilder` → `Graph::Execute` → `MetaPipeline` → `PreparedQuery` →
`PipelineExecutor`. Operators on it: TableScan, Filter, Project, Limit, Union, Collector.

## Open decisions (owner)

1. **Legacy callback engine.** `QueryBuilder::Execute`/`ExecuteCollect` still run the old
   push-callback `PhysicalPlan` engine. Removing it makes `ExecuteOnGraph` the only path and
   drops `ExecuteWithControl` (early cancel has no taskflow equivalent yet). Recommend: delete.
2. **Aggregate expressions** (~400 LOC, no plan node, no physical mapping). Phase 2.7 in
   `tasks/todo.md` plans an Aggregate operator. Recommend: keep until 2.7 decides.
3. **Runtime-tuning + flow control** (~400 LOC: `EdgeRef`, `Graph::Tune`, `SetFlowControl`,
   `WaitIfSaturated`). The engine never calls it, so back-pressure is inert today.
   Recommend: either wire `WaitIfSaturated` into `Pipeline` or delete the whole surface.
4. **Parquet reader** (~2.5k LOC). It is the only `DataSource`, and the `DataSource` interface
   is coupled to it. Recommend: keep, fix its bounds checks (stage 2).
5. **Header guards → `#pragma once`.** Removes the last macros but contradicts the CLAUDE.md
   rule. Recommend: switch and update CLAUDE.md.

## Stages

**S1 — Delete dead code (~490 LOC, 0 references).**
`Graph::PrintStructure`/`PrintBackwardsFrom`; the row-estimation cluster across
graph/operator/port; `Operator::MessageId`; `Port::owner`/`AddPorts`; unused `operator_type`
helpers; `EdgeConfig::Simple`; `ChainBuilder::Done`; `PipelineTask` counters; dead
`BlockingQueue` members (`PopWithTimeout`, `WaitForNext`, `FifoQueue`, `max_bytes`/`max_batches`);
`RowGroupIterator`/`ScanRowGroups`; `DictionaryDecoder`; `DecodeDefinitionLevels`;
`CompressionTypeName`; `ReadDouble`/`HasMore`; write-only fields (also fixes the
`page_reader.hpp:392` warning).

**S2 — Correctness fixes found during audit.**
- `Status::Invalid` etc. record `result.hpp:145` as the error location for every error; forward
  a `std::source_location` parameter. Print error codes by name, not int.
- Parquet decoders do unchecked `subspan`/`memcpy` with lengths read from the file
  (`page_reader.hpp:420`, `column_reader.hpp:166`, `encoding.hpp:63`). Add one bounds-checked
  `LoadLE<T>(span, pos) -> Result<T>` using `std::bit_cast` + `std::byteswap`.
- `BlockingQueue` signals EOS as `Status::Invalid`; return `Result<std::optional<T>>`.

**S3 — Remove legacy paths with their tests (decision 1).**
Move `CreatePhysicalExpr` next to `execution/graph_builder`, then delete `PhysicalPlan`,
`*Exec`, `CreatePhysicalPlan`, `StreamingQueryBuilder`, `Generator`, `BloomFilter`, and the
callback `QueryBuilder` methods. Migrate the tests that use `ExecuteCollect` as a parity oracle
to `ExecuteOnGraph`; delete test_streaming_query_builder, test_generator, test_bloom_filter,
examples `bloom_join*` and `logical_plan_to_physical_plan_demo`.

**S4 — Remove macros.**
`QASSERT`/`GUARD_ASSIGN`/`GUARD_OK` have 0 uses in `src/`: delete them and their tests.
Standard propagation becomes `if (!r) return std::unexpected(std::move(r).error());`, with
`and_then`/`transform_error` for linear chains. Header guards per decision 5. Test macros
`ASSERT_OK*` stay (gtest needs them) but move to `tests/support/` and drop reserved `_QUEST_`
names.

**S5 — Error model cleanup.**
`using Status = std::expected<void, Error>;` instead of inheriting from it (codemod: 141
factory calls, 57 `Status::OK()`, 51 `.ok()`). Replace the ~24 `throw`s (serializer, schema,
record_batch, memory_resource, graph, port) with `Result` returns.

**S6 — Reorganize.**
```
src/quest/
  common/         result.hpp
  columnar/       types, schema, column_vector, record_batch, record_batch_serializer
  io/parquet/     file/page/column readers, encoding, thrift, metadata, compression
  datasource/     data_source.hpp (interface only, decoupled from Parquet)
  memory/  cache/
  plan/logical/  plan/physical/expressions/
  execution/kernels/    batch_kernels
  execution/lowering/   plan_to_graph.{hpp,cpp}  (was execution/graph_builder.hpp)
  taskflow/ (+operators/)   builder/   quest.hpp (user-facing only)
tests/support/test_utils.hpp
```
Break the `cache → batch_kernels → physical_expr` include cycle by moving
`ConcatenateHostBlocks` into `columnar/`. Stop including `quest.hpp` internally.
Done (2026-10-31): `ConcatenateRecordBatches` went to `columnar/` and `ConcatenateHostBlocks` stayed in `cache/` (it unwraps host blocks); lowering is header-only (`plan_to_graph.cpp` deferred to S7); logical `Scan` keeps its `projection` parameter because `LogicalPlanTest.ScanWithProjection` passes one.

**S7 — C++ quality (by payoff).**
- Move bodies of the largest headers to `.cpp` (`metadata`, `physical_expr`, `column_reader`,
  `thrift_decoder`, `batch_kernels`, lowering): today ~12k of 15k LOC is header-only.
- Collapse the per-operator binary-expression classes into one `BinaryExpr` + `enum class
  BinaryOp`, removing 13 chained `dynamic_cast`s in lowering.
- `std::to_underlying`, `std::format`, `std::unreachable`, `std::move_only_function` for
  callbacks, `constexpr std::array` name tables instead of switches.
- CMake: `target_compile_options`, `-Werror` in CI, `Threads::Threads`, fix
  `project(bigger-than-gpu)` name, `QUEST_SANITIZERS` as a STRING cache var. The code compiles
  under `-std=c++23`, so the standard can drop from 26 to 23.

## Resolved decisions (execution, 2026-10-31)

Guiding rule: keep every component a Sirius-style CPU query engine needs (scan, filter, project,
aggregate, join, sort, limit, spill, back-pressure, Parquet).

1. Legacy callback engine: **delete**, except `BloomFilter`, its test and the `bloom_join`
   example. The filter is standalone (no legacy-engine dependency) and is the runtime-filter
   building block for hash joins.
2. Aggregate expressions: **keep** (needed for the Aggregate operator).
3. Runtime tuning + flow control: **keep, not wired**. Pipelines behind a barrier run after their
   producer finishes, so blocking in `WaitIfSaturated` there would deadlock; spill already covers
   that case. Back-pressure becomes live once concurrently running pipelines (exchange/shuffle on
   the `distribution` branch) share an edge.
4. Parquet reader: **keep**, fix bounds checks (S2).
5. Header guards: **switch to `#pragma once`**, update CLAUDE.md.

S1 adjustments: keep `AddPorts` (multi-input join ports). Row estimation is deleted because
`QueryStats` is the live cardinality input.

Regression gate per stage: full build, all ctest tests (baseline 609; 
`GraphBridgeTest.FlagshipSpillThroughRealOperators` is a pre-existing flake under `ctest -j`),
and byte-identical example/Parquet-decoder outputs against the pre-change binaries.
