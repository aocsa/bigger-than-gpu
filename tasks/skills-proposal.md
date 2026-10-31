# Skills Proposal for QUEST

## The Pipeline: lessons.md → rules → skills

The tutorial describes a learning loop:
1. Claude makes a mistake → user corrects → lesson logged in `tasks/lessons.md`
2. Lessons that repeat become **rules** in CLAUDE.md
3. Rules that need multi-step workflows become **skills** in `.claude/skills/`

Right now we have zero lessons and zero skills. But we DON'T need to wait for
mistakes — we already know QUEST's pain points from `future_improvement.md`,
the architecture, and the prompts.md patterns.

## Proposed Skills (ranked by impact)

### 1. `new-operator` — Operator Scaffold Generator

**Why**: QUEST has 19 operator types in the enum but only 3 with full
logical+physical plan implementations. Every new operator follows the exact
same checklist. This is the highest-ROI skill.

**Trigger**: "new operator", "add operator", "create operator", "implement X operator"

**What it does**:
1. Ask: name, role (streaming/source/blocking), port count, transform description
2. Generate: enum entry, operator subclass with pipeline-role methods (`ProcessBatch` /
   `IsSource`+`GetNextBatch` / `IsSink`+`Sink`+`FinalizeSink` as applicable)
3. Generate: test file wired via `GraphBuilder`/`PreparedQuery` with empty/null/EOS/backpressure cases
4. Register in CMakeLists.txt
5. Build and verify

**Source pattern**: Prompts.md Example 4 (New Operator Scaffold)

---

### 2. `verify-operator` — Adversarial Operator Tester

**Why**: The prompts.md verification agent pattern applied to QUEST. New operators
get written but rarely stress-tested against the specific failure modes that
matter: backpressure, spill boundaries, null columns, concurrent access.

**Trigger**: "verify operator", "test operator", "break this operator"

**What it does**:
1. Empty/null DataBlocks through the operator
2. Backpressure: downstream BatchHolder full
3. Spill boundary: force 84% → 86% memory mid-operation
4. Type edge cases: NULL columns, zero-length strings, INT64_MAX
5. Concurrent access: two operators sharing a BatchHolder (via Graph::Execute)
6. Pipeline role contract: does each role method (`ProcessBatch`/`GetNextBatch`/`Sink`/`FinalizeSink`) satisfy its invariants?
7. Verdict: PASS / PARTIAL / FAIL

**Source pattern**: Prompts.md Example 3 (Adversarial Operator Verifier)

---

### 3. `trace-dataflow` — Read-Only Data Flow Tracer

**Why**: Debugging QUEST means tracing through 6 layers. The debugging trace order
is in CONTRIBUTING.md but it's just a file list. This skill would actually TRACE
a query through all layers and show what happens at each stage.

**Trigger**: "trace", "follow the data", "how does this query execute", "debug query"

**What it does** (read-only, uses subagent):
1. Parse the QueryBuilder DSL call
2. Show which LogicalPlan nodes are created
3. Show logical → physical mapping
4. Show TaskFlow graph wiring (operators, edges, ports)
5. Show BatchHolder types chosen and why
6. Estimate memory pressure and spill likelihood

**Source pattern**: Prompts.md Example 7 (Data Flow Tracer)

---

### 4. `new-plan-node` — Plan Node Generator

**Why**: The biggest gap in QUEST is missing plan nodes (Limit, Aggregate, Sort, Join).
Each requires BOTH a logical plan node AND a physical plan node, plus expression
support. The dependency chain is non-obvious.

**Trigger**: "add plan node", "implement Limit", "add Aggregate", "new logical plan"

**What it does**:
1. Create logical plan node in `src/quest/logical_plan/`
2. Create physical plan node in `src/quest/physical_plan/`
3. Add expression support if needed
4. Wire into QueryBuilder/QueryPlan fluent API
5. Add test coverage for both logical and physical
6. Update `future_improvement.md` to mark as done

**Source pattern**: Custom (derived from query_builder_refactor.md architecture)

---

### 5. `review-memory-safety` — Memory Safety Reviewer

**Why**: QUEST's ownership model is the #1 source of potential bugs. shared_ptr vs
unique_ptr choices have specific reasons. A reviewer that knows the ownership table
and checks new code against it would catch issues before they become data races.

**Trigger**: "review memory", "check ownership", "review safety", before any PR

**What it does** (read-only):
1. Check all new/modified pointer usage against ownership model
2. Flag any raw new/delete
3. Verify BatchHolder sharing between source/target operators
4. Check spill-to-disk paths release CPU memory correctly
5. Look for data races in BlockingQueue interactions
6. Verdict with file:line references

**Source pattern**: Prompts.md Example 2 (Memory Safety Reviewer agent)

---

## Skills NOT Proposed (and why)

| Idea | Why Skip |
|------|----------|
| Build/test runner | Too simple — just `cmake --build build && ctest`. Not worth a skill. |
| Code formatter | `.clang-format` already handles it. |
| Architecture explainer | `docs/dev/ARCHITECTURE.md` is comprehensive. A skill would just parrot it. |
| Compaction template | Engram already handles session persistence. |

## File Structure

```
.claude/skills/
├── new-operator/
│   └── SKILL.md
├── verify-operator/
│   └── SKILL.md
├── trace-dataflow/
│   └── SKILL.md
├── new-plan-node/
│   └── SKILL.md
└── review-memory-safety/
    └── SKILL.md
```

## Implementation Order

1. **`new-operator`** — highest ROI, needed for every P4 item in future_improvement.md
2. **`new-plan-node`** — enables Limit/Aggregate/Sort/Join implementation
3. **`verify-operator`** — quality gate for #1 and #2
4. **`review-memory-safety`** — catches ownership bugs in any PR
5. **`trace-dataflow`** — nice-to-have for debugging

## Lessons → Rules → Skills Pipeline

Once we start using these skills, the feedback loop works like this:

```
User corrects Claude → tasks/lessons.md
  ↓ (pattern repeats 2-3 times)
Lesson graduates to rule → CLAUDE.md
  ↓ (rule needs multi-step workflow)
Rule graduates to skill → .claude/skills/
```

Example: if Claude keeps forgetting to add the CMakeLists.txt registration for
new test files, that becomes a lesson. After it happens twice, it becomes a rule
in CLAUDE.md. If the registration has 4+ steps, it becomes part of a skill.
