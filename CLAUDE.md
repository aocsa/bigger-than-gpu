# QUEST — C++26 Batch Processing Framework

Two-tier memory (CPU/Disk) batch processing engine inspired by BlazingSQL.

## Codebase Map

Static library `quest` (`quest::quest`), built from the `.cpp` files listed in root `CMakeLists.txt`
(`QUEST_SOURCES`; most of the code is header-only). Deps: Snappy, pthread; GTest for tests.
Toolchain is pinned via Pixi (`pixi.toml`, see `docs/dev/cpp26-pixi-setup.md`). Options:
`QUEST_BUILD_TESTS`, `QUEST_BUILD_EXAMPLES`, `QUEST_SANITIZERS` (asan/tsan/ubsan). Default build type: Debug.

Source lives in `src/quest/`; namespaces follow directories (`quest::columnar`, `quest::io::parquet`,
`quest::plan::logical`, ...). `quest.hpp` is the user-facing umbrella header and is never included
inside `src/`; the error type is `common/result.hpp` (namespace `quest`).

| Dir | Role |
|---|---|
| `builder/` | User-facing fluent API: `QueryBuilder`, `ExprBuilder`, `QueryPlan` (`quest`) |
| `common/` | `Result`, `Status`, `Error` (`quest`) |
| `columnar/` | Types, schema, column vectors, record batches + serializer, `ConcatenateRecordBatches` (`quest::columnar`) |
| `datasource/` | `DataSource` interface: `schema()`, `num_partitions()`, `ReadPartition()` (`quest::datasource`) |
| `io/parquet/` | Parquet file/page/column readers, encoding, thrift, metadata, compression, `ParquetDataSource` (`quest::io::parquet`) |
| `plan/logical/` | Logical plan nodes and expressions (`quest::plan::logical`) |
| `plan/physical/` | Physical expressions: `expressions/physical_expr.hpp` (`quest::plan::physical`) |
| `execution/kernels/` | Batch filter/project/truncate kernels, bloom filter (`quest::execution`) |
| `execution/lowering/` | `plan_to_graph.hpp`: `CreateGraph`, `CreatePhysicalExpr` (`quest::execution`) |
| `taskflow/` | Execution engine: `Graph`, `GraphBuilder`, `Pipeline`, `MetaPipeline`, `PipelineExecutor`, `PreparedQuery`, `Port`, `Operator`, `OperatorType` (`quest::taskflow`) |
| `taskflow/operators/` | Concrete operators: TableScan, Generate, Filter, Project, Limit, Union, Print, Collector |
| `cache/` | `DataBlock`, `BatchHolder` (CPU/Disk tiers, spill), `BlockingQueue`, host-block helpers (`quest::cache`) |
| `memory/` | `MemoryResourceManager` (`quest::memory`) |

Query flow: `QueryBuilder::ExecuteOnGraph` -> logical plan -> `execution::CreateGraph` -> `Graph` of
`Operator`s split into `Pipeline`s by `MetaPipeline` -> `PipelineExecutor` runs them
(sources `GetNextBatch`, streaming ops `ProcessBatch`, blocking ops `Sink`/`FinalizeSink`).

`OperatorType` also reserves Aggregate, Sort, Join, Partition and Output entries that have no
operator class yet; check `operators/` before assuming one exists.

Other dirs: `tests/` (one `test_*.cpp` per area, GTest), `examples/`, `docs/`, `tasks/`
(todo, lessons, handoff notes), `.claude/skills/` (new-operator, verify-operator, etc.).

## Mandatory Rules

### Ownership Model
- `unique_ptr`: expression trees, plan nodes, compression codecs, column vectors
- `shared_ptr`: IBatchHolder, MemoryResourceManager, DataSource, ColumnVector, DataBlock
- `weak_ptr`: operator → Graph back-references
- ZERO raw `new`/`delete`. Use `make_unique`/`make_shared` EXCLUSIVELY.

### Error Handling
- Use `std::expected` (Result/Status types). NEVER throw exceptions.
- Propagate errors with early returns, not nested if-chains.

### Naming Conventions
- Classes/Structs: PascalCase (`BatchHolder`, `DataBlock`)
- Methods: PascalCase (`Execute`, `ProcessBatch`)
- Private members: snake_case with trailing `_` (`plan_`, `input_port_`)
- Namespaces: snake_case (`quest::taskflow`, `quest::cache`)
- Files: snake_case (`batch_holder.hpp`, `operator_type.hpp`)
- Type aliases: PascalCase + Ptr/Type suffix (`LogicalPlanPtr`, `DataBlockPtr`)
- Constants: `k`-prefix PascalCase (`kGraphEntryPointId`)
- Enum values: PascalCase for execution layer, `k`-prefix for Parquet/columnar layer

### Code Format & Quality
- Style: `.clang-format` (Google-based, 100 col, 2-space indent)
- `[[nodiscard]]` on: getters, query methods, factory functions, Result-returning functions
- `noexcept` on: trivial getters, identity methods
- `const`-qualify all methods that don't mutate state

### Header Guards
- Use `#pragma once`, placed after the copyright header
- No macros in `src/`; test-only macros (`ASSERT_OK`, `ASSERT_OK_AND_ASSIGN`) live in
  `tests/support/test_utils.hpp`

### Documentation
- Public API: Doxygen `///` with `@brief`, `@param`, `@return`
- Section separators: `// ====...====` banners
- New files MUST use Doxygen. Don't reformat existing `//` comments unless touching that code.

### Copyright
- New files: `// Copyright 2024 Quest Authors` + `// SPDX-License-Identifier: Apache-2.0`

### Before Writing Code
1. Check ownership conventions match the model above
2. New operators MUST implement the pipeline-role contract:
   - **Streaming transformations** (Filter, Project, Limit): override `ProcessBatch(DataBlockPtr)` — called in-line by `Pipeline::RunTask`; no `run()` loop.
   - **Sources** (TableScan, Generate, BindableTableScan): override `GetNextBatch()` and ensure `IsSource()` is true, either through `OperatorType` classification or an override. Return `std::optional<DataBlockPtr>` (nullopt = EOS, never `Status::Invalid`).
   - **Blocking operators** (Aggregate, Sort, Join): override `Sink(DataBlockPtr)` and `FinalizeSink()`; expose `IsSource()` + `GetNextBatch()` only after finalization for the post-barrier source phase.
   - All roles default to passthrough/no-op in `Operator`; override only what the operator needs.
3. Every new file gets a test in `tests/` registered in `tests/CMakeLists.txt`
4. `QueryBuilder` methods are rvalue-qualified (`&&`) — chain with `std::move`

## Workflow

### Plan Mode Default
- Enter plan mode for ANY non-trivial task (3+ steps or architectural decisions)
- If something goes sideways, STOP and re-plan immediately — don't keep pushing
- Use plan mode for verification steps, not just building
- Write detailed specs upfront to reduce ambiguity

### Subagent Strategy
- Use subagents liberally to keep main context window clean
- Offload research, exploration, and parallel analysis to subagents
- For complex problems, throw more compute at it via subagents
- One task per subagent for focused execution

### Self-Improvement Loop
- After ANY correction from the user: update `tasks/lessons.md` with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

### Verification Before Done
- Never mark a task complete without proving it works
- Diff behavior between main and your changes when relevant
- Ask yourself: "Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness

### Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"
- Skip this for simple, obvious fixes — don't over-engineer
- Challenge your own work before presenting it

### Autonomous Bug Fixing
- When given a bug report: just fix it. Don't ask for hand-holding
- Point at logs, errors, failing tests — then resolve them
- Zero context switching required from the user
- Go fix failing CI tests without being told how

## Task Management

1. **Plan First**: Write plan to `tasks/todo.md` with checkable items
2. **Verify Plan**: Check in before starting implementation
3. **Track Progress**: Mark items complete as you go
4. **Explain Changes**: High-level summary at each step
5. **Document Results**: Add review section to `tasks/todo.md`
6. **Capture Lessons**: Update `tasks/lessons.md` after corrections

## Core Principles

- **Simplicity First**: Make every change as simple as possible. Impact minimal code.
- **No Laziness**: Find root causes. No temporary fixes. Senior developer standards.
- **Minimal Impact**: Changes should only touch what's necessary. Avoid introducing bugs.

## Build & Test

- Build: `cmake --build build`
- Test all: `ctest --test-dir build --output-on-failure`
- Test one: `./build/tests/test_{name}`
- Test filter: `./build/tests/test_{name} --gtest_filter="*Pattern*"`
- Clean build (after CMake changes): `rm -rf build && cmake -B build && cmake --build build`

## Context

For architecture, class diagrams, and design patterns: `docs/dev/ARCHITECTURE.md`
For build commands, directory map, and debugging trace: `docs/dev/CONTRIBUTING.md`
For known bugs and roadmap: `docs/dev/future_improvement.md`
