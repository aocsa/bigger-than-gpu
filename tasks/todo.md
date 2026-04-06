# Task: Implement 5 QUEST Skills in `.claude/skills/`

## Goal

Create 5 project-specific skills that teach Claude how to perform common QUEST development tasks. Each skill is a folder with a `SKILL.md` file inside `.claude/skills/`.

## Context

QUEST is a C++23 batch processing framework with CPU/Disk memory tiers. The codebase has:

- **19 operator types** in the enum (`src/quest/taskflow/operator_type.hpp`) but only **3 with full logical+physical plan implementations** (Scan, Selection, Projection)
- **Two base classes** for operators: `Operator` (stateful) and `SimpleOperator` (stateless transform with automatic `run()` loop) in `src/quest/taskflow/operator.hpp`
- **Logical plan** nodes in `src/quest/logical_plan/logical_plan.hpp` — `Scan`, `Selection`, `Projection`
- **Physical plan** nodes in `src/quest/physical_plan/physical_plan.hpp` — `ScanExec`, `SelectionExec`, `ProjectionExec`
- **Logical→Physical translation** via `CreatePhysicalPlan()` factory function in `physical_plan.hpp` (line ~439)
- **QueryPlan** fluent API in `src/quest/builder/query_plan.hpp` — `Where()`, `Select()`, all `&&`-qualified
- **Ownership model**: `unique_ptr` for plan nodes/expressions, `shared_ptr` for BatchHolder/DataSource/DataBlock, `weak_ptr` for operator→Graph
- **Known bugs and gaps**: documented in `docs/dev/future_improvement.md`

## Skill Format

Each skill is a folder with one file:

```
.claude/skills/{skill-name}/SKILL.md
```

The `SKILL.md` has YAML frontmatter (name, description) and a Markdown body with instructions. See `tasks/skills-proposal.md` for the full design rationale and the tutorial at `tutorial.md` for the skill creation guide.

## Tasks

### Pre-work

- [x] Read the skill creation tutorial in `tutorial.md` (the "How to Create Your Own Agent Skills" section)
- [x] Read `tasks/skills-proposal.md` for design rationale and what NOT to build
- [x] Verify `.claude/skills/` directory exists (create if not — may need sandbox bypass for `.claude/`)

### Skill 1: `new-operator`

- [x] Create `.claude/skills/new-operator/SKILL.md`

**What it does**: Scaffolds a new QUEST operator with all required files.

**Trigger keywords**: "add operator", "new operator", "create operator", "implement X operator"

**Must reference these source patterns**:
- `Operator` base class: `src/quest/taskflow/operator.hpp` — constructor signature `(uint32_t id, string expression, OperatorType type)`, pure virtual `run()` and `can_you_throttle_my_input()`
- `SimpleOperator` template: same file — extends `Operator`, provides `run()` loop, derived classes only implement `ProcessBatch(shared_ptr<DataBlock>)`
- `OperatorType` enum: `src/quest/taskflow/operator_type.hpp` — 19 entries, plus helper functions `is_source_operator()`, `is_terminal_operator()`, `is_multi_input_operator()`
- Test registration: `tests/CMakeLists.txt` — pattern is `add_executable` + `target_link_libraries(... PRIVATE quest::quest GTest::gtest GTest::gtest_main)` + `gtest_discover_tests`
- DSL wiring: `graph += scan >> new_op >> output;`

**Steps the skill must define**:
1. Ask: name, stateless vs stateful, port count, description (then STOP)
2. Add enum entry + `to_string()` case + helper function if needed
3. Create operator class file in `src/quest/taskflow/`
4. Create test file in `tests/`
5. Register test in `tests/CMakeLists.txt`
6. Build and verify
7. Show DSL usage example

**Gotchas to include**:
- Operators are non-copyable AND non-movable (both deleted in base)
- `owning_graph_` is `weak_ptr` — must `.lock()` before use
- Multi-input operators need named ports via `input()["port_name"]`
- Sentinel node id is `-1`

### Skill 2: `new-plan-node`

- [x] Create `.claude/skills/new-plan-node/SKILL.md`

**What it does**: Adds a new logical+physical plan node and wires it into QueryPlan.

**Trigger keywords**: "add plan node", "implement Limit", "add Aggregate", "new logical plan"

**Must reference these source patterns**:
- `LogicalPlan` base: `src/quest/logical_plan/logical_plan.hpp` — pure virtual `schema()`, `Children()`, `ToString()`
- Existing nodes: `Scan` (lines 67-122), `Selection` (129-150), `Projection` (157-198) in same file
- `PhysicalPlan` base: `src/quest/physical_plan/physical_plan.hpp` — pure virtual `schema()`, `Execute(ResultCallback)`, `Children()`, `ToString()`
- Existing physical: `ScanExec`, `SelectionExec`, `ProjectionExec` in same file
- `CreatePhysicalPlan()` factory: `physical_plan.hpp` line ~439 — add new `dynamic_cast` branch
- `CreatePhysicalExpr()` factory: same file line ~373 — if new expressions needed
- `QueryPlan` fluent API: `src/quest/builder/query_plan.hpp` — add new `&&`-qualified method
- Ownership: `LogicalPlanPtr = unique_ptr<LogicalPlan>`, `PhysicalPlanPtr = unique_ptr<PhysicalPlan>`, `LogicalExprPtr = unique_ptr<LogicalExpr>`

**Steps the skill must define**:
1. Ask: node name, input arity, does it need new expression types? (then STOP)
2. Create logical plan node class in `logical_plan.hpp`
3. Create physical plan node class in `physical_plan.hpp`
4. Add branch in `CreatePhysicalPlan()` factory
5. Add fluent method to `QueryPlan` (e.g., `.Limit(n)`)
6. Add tests for both logical and physical nodes
7. Build and verify

**Gotchas to include**:
- All `QueryPlan` methods are `&&`-qualified — must return by value via `QueryPlan(std::make_unique<NewNode>(std::move(plan_), ...))`
- `CreatePhysicalPlan` uses `dynamic_cast` chains — add new branch BEFORE the final error return
- Expressions use `unique_ptr` everywhere — no `shared_ptr` for expression trees
- Physical `Execute()` must propagate errors via `std::expected`, not exceptions

### Skill 3: `verify-operator`

- [x] Create `.claude/skills/verify-operator/SKILL.md`

**What it does**: Adversarial testing that tries to BREAK an operator, not confirm it works.

**Trigger keywords**: "verify operator", "test operator", "break this operator", "stress test"

**Must reference these patterns**:
- `BatchHolder` interface: `src/quest/cache/batch_holder.hpp` — `Add()`, `Pull()`, `Finish()`
- `DataBlock` types: `src/quest/cache/data_block.hpp` — `HostMemoryBlock`, `DiskBlock`
- `BlockingQueue`: `src/quest/cache/blocking_queue.hpp` — thread safety via mutex + condition_variable
- Memory saturation: `src/quest/memory/memory_resource.hpp` — 85% CPU threshold, `is_saturated()`, `should_spill()`
- Known bugs from `docs/dev/future_improvement.md`: data race in `Graph::Execute()`, busy-wait spin, `IsNull()` always false

**6-point test strategy**:
1. Empty/null DataBlocks through the operator
2. Backpressure: downstream BatchHolder full
3. Spill boundary: force 84% → 86% memory mid-operation
4. Type edge cases: NULL columns, zero-length strings, INT64_MAX
5. Concurrent access: two operators sharing a BatchHolder
6. Throttle compliance: does `can_you_throttle_my_input()` actually work?

**Verdict format**: PASS / PARTIAL / FAIL with file:line references

### Skill 4: `trace-dataflow`

- [x] Create `.claude/skills/trace-dataflow/SKILL.md`

**What it does**: Read-only trace of how a query flows through all 6 QUEST layers. Uses a subagent.

**Trigger keywords**: "trace", "follow the data", "how does this query execute", "debug flow"

**Must reference these layers in order**:
1. `src/quest/builder/query_plan.hpp` — QueryBuilder DSL, `Where()`, `Select()`
2. `src/quest/logical_plan/logical_plan.hpp` — which nodes created
3. `src/quest/physical_plan/physical_plan.hpp` — `CreatePhysicalPlan()` mapping
4. `src/quest/taskflow/graph.hpp` — operator wiring, BFS scheduling
5. `src/quest/cache/batch_holder.hpp` — Simple vs Concatenating, flow control
6. `src/quest/memory/memory_resource.hpp` — tier recommendation, spill decision

**Output format**: nested tree showing the query at each layer, ending with memory estimate.

**Key rule**: This skill is READ-ONLY. It must NOT modify any file. Use a subagent for the exploration.

### Skill 5: `review-memory-safety`

- [x] Create `.claude/skills/review-memory-safety/SKILL.md`

**What it does**: Reviews code changes against QUEST's ownership model. Checks for pointer misuse, data races, and memory leaks.

**Trigger keywords**: "review memory", "check ownership", "review safety", "memory review"

**Must check against this ownership table**:
- `unique_ptr`: expression trees (`LogicalExprPtr`), plan nodes (`LogicalPlanPtr`, `PhysicalPlanPtr`), compression codecs, column vectors
- `shared_ptr`: `IBatchHolder` (shared between source/target operators), `MemoryResourceManager` (one per graph), `DataSource` (shared across plan layers), `ColumnVector` in `RecordBatch` (zero-copy slicing), `DataBlock` (multi-consumer cache)
- `weak_ptr`: `Operator::owning_graph_` (back-reference to Graph)

**Checklist**:
1. Any raw `new`/`delete` → FAIL
2. `shared_ptr` where `unique_ptr` suffices → WARNING
3. Missing `.lock()` on `weak_ptr` before use → FAIL
4. `shared_ptr` cycles (A→B→A) → FAIL
5. `DataBlock` not released after spill-to-disk → WARNING
6. Atomic access without proper memory ordering → WARNING
7. `BlockingQueue` accessed without mutex → FAIL

**Known data races** (from `docs/dev/future_improvement.md`):
- `processed_edges` in `Graph::Execute()` — read in jthread lambdas, written in main loop
- `saturation_threshold` — written under mutex, read without it in `is_saturated()`

**Verdict format**: PASS / WARNING / FAIL with file:line references

## Implementation Notes

- Each `SKILL.md` must be under 500 lines (ideally under 200)
- Frontmatter `description` is the MOST important field — it determines when the skill triggers
- Don't teach Claude C++ syntax — focus on QUEST-specific conventions and gotchas
- Include "Read these files first" section pointing to actual source files
- Include a "Gotchas" section with non-obvious failure modes
- All skills should reference `docs/dev/future_improvement.md` for known issues

## Verification

After creating all 5 skills, verify:
- [x] Each folder exists under `.claude/skills/`
- [x] Each `SKILL.md` has valid YAML frontmatter with `name` and `description`
- [x] Descriptions include trigger keywords
- [x] No skill exceeds 500 lines (max: 165 lines)
- [x] No skill duplicates content from CLAUDE.md or docs/dev/

## Reference Files

| File | Why You Need It |
|------|----------------|
| `src/quest/taskflow/operator.hpp` | Operator + SimpleOperator base classes |
| `src/quest/taskflow/operator_type.hpp` | OperatorType enum (19 types) |
| `src/quest/logical_plan/logical_plan.hpp` | Scan, Selection, Projection nodes |
| `src/quest/physical_plan/physical_plan.hpp` | Physical nodes + CreatePhysicalPlan() factory |
| `src/quest/builder/query_plan.hpp` | QueryPlan fluent API |
| `src/quest/cache/batch_holder.hpp` | IBatchHolder interface |
| `src/quest/cache/data_block.hpp` | HostMemoryBlock, DiskBlock |
| `src/quest/memory/memory_resource.hpp` | MemoryResource, saturation thresholds |
| `docs/dev/future_improvement.md` | Known bugs, missing features, priorities |
| `docs/dev/ARCHITECTURE.md` | Full architecture reference |
| `tests/CMakeLists.txt` | Test registration pattern |
| `tutorial.md` | Skill creation guide |
| `tasks/skills-proposal.md` | Design rationale |
