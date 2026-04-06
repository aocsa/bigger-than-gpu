# Plan: Lessons → Rules → Skills → Commands Pipeline

## Current State

```
.claude/
├── settings.local.json
└── skills/                          # 5 skills created (previous session)
    ├── new-operator/SKILL.md        # Scaffold new QUEST operator
    ├── new-plan-node/SKILL.md       # Add logical+physical plan node
    ├── verify-operator/SKILL.md     # Adversarial operator testing
    ├── trace-dataflow/SKILL.md      # Read-only 6-layer query trace
    └── review-memory-safety/SKILL.md # Ownership model reviewer

tasks/
├── todo.md           # Implementation plan (current)
├── lessons.md        # Empty — no lessons yet
└── skills-proposal.md # Design rationale

CLAUDE.md             # Rules + workflow (72 lines)
```

**Missing**: `.claude/commands/` directory. No commands exist yet.

## The Full Pipeline

```
mistakes happen
       ↓
tasks/lessons.md        ← log the pattern + prevention rule
       ↓ (repeats 2-3x)
CLAUDE.md               ← promote to a permanent rule
       ↓ (rule needs multi-step workflow)
.claude/skills/         ← encode as skill with triggers + steps
       ↓ (need deterministic invocation)
.claude/commands/       ← slash command that launches the skill
```

Each level adds RELIABILITY:
- **Lessons**: Claude reads at session start, MAY follow
- **Rules in CLAUDE.md**: Claude reads at every message, SHOULD follow
- **Skills**: Claude activates when description matches, MIGHT trigger
- **Commands**: User types `/command`, ALWAYS triggers (deterministic)

The key insight from the tutorial: **skills can't be invoked deterministically** — Claude decides whether to activate them based on the description match. Commands CAN be invoked deterministically. So the pattern is: **skill holds the knowledge, command is the guaranteed trigger**.

## What Needs to Be Done

### Phase 1: Commands for Existing Skills

Create `.claude/commands/` with slash commands that deterministically trigger each skill.

Commands are markdown files in `.claude/commands/`. The filename becomes the slash command:
- `.claude/commands/new-op.md` → user types `/new-op`
- The file contents become the prompt that Claude receives

#### Command 1: `/new-op`

File: `.claude/commands/new-op.md`

Purpose: Deterministic trigger for the `new-operator` skill.

Content should:
1. Tell Claude to read and follow `.claude/skills/new-operator/SKILL.md`
2. Accept an optional argument for the operator name: `/new-op Sort`
3. If no argument, the skill will ask

#### Command 2: `/new-node`

File: `.claude/commands/new-node.md`

Purpose: Deterministic trigger for the `new-plan-node` skill.

Content should:
1. Tell Claude to read and follow `.claude/skills/new-plan-node/SKILL.md`
2. Accept optional argument: `/new-node Limit`
3. Reference `docs/dev/future_improvement.md` P4 for the implementation order

#### Command 3: `/verify`

File: `.claude/commands/verify.md`

Purpose: Deterministic trigger for the `verify-operator` skill.

Content should:
1. Tell Claude to read and follow `.claude/skills/verify-operator/SKILL.md`
2. Accept operator name argument: `/verify SortOperator`
3. Use a subagent for the actual testing

#### Command 4: `/trace`

File: `.claude/commands/trace.md`

Purpose: Deterministic trigger for the `trace-dataflow` skill.

Content should:
1. Tell Claude to read and follow `.claude/skills/trace-dataflow/SKILL.md`
2. Accept a query snippet: `/trace QueryBuilder::FromParquet("x").Where(Col("a") > Lit(5))`
3. Enforce read-only — the command must NOT modify files

#### Command 5: `/review-mem`

File: `.claude/commands/review-mem.md`

Purpose: Deterministic trigger for the `review-memory-safety` skill.

Content should:
1. Tell Claude to read and follow `.claude/skills/review-memory-safety/SKILL.md`
2. Accept optional file paths: `/review-mem src/quest/taskflow/sort_operator.hpp`
3. If no argument, review all uncommitted changes (`git diff`)

### Phase 2: `!command` Dynamic Context Injection

The `!command` syntax inside `SKILL.md` lets you embed shell commands that run at skill load time and inject their output into the prompt. This is powerful for:

- Injecting current git state into a review skill
- Showing current test results before verification
- Listing available operator types dynamically

#### Update `verify-operator/SKILL.md`

Add dynamic context at the top:

```markdown
## Current State

!`git diff --name-only HEAD~1`

## Available Operator Types

!`grep -A1 'enum class OperatorType' src/quest/taskflow/operator_type.hpp | head -30`
```

This means when the skill activates, Claude sees the ACTUAL current diff and operator types, not a static reference.

#### Update `review-memory-safety/SKILL.md`

Add:

```markdown
## Files Changed

!`git diff --name-only`

## Current Ownership Usage

!`grep -rn 'shared_ptr\|unique_ptr\|weak_ptr\|make_shared\|make_unique' src/quest/ --include='*.hpp' | head -40`
```

#### Update `new-operator/SKILL.md`

Add:

```markdown
## Existing Operator Types

!`grep -A30 'enum class OperatorType' src/quest/taskflow/operator_type.hpp`
```

So when creating a new operator, Claude sees the current enum without needing to read the file separately.

#### Update `new-plan-node/SKILL.md`

Add:

```markdown
## Existing Plan Nodes

!`grep 'class.*: public LogicalPlan' src/quest/logical_plan/logical_plan.hpp`
!`grep 'class.*: public PhysicalPlan' src/quest/physical_plan/physical_plan.hpp`
```

### Phase 3: Lessons Structure

The `tasks/lessons.md` file needs a clear format so lessons can be promoted to rules/skills.

#### Lesson Format

```markdown
## Lesson #{N}: {title}
**Date**: YYYY-MM-DD
**What happened**: {description of the mistake}
**Root cause**: {why it happened}
**Rule**: {the prevention rule — actionable, specific}
**Promoted to**: {none | CLAUDE.md | skill:{skill-name}}
```

#### Promotion Criteria

A lesson gets promoted when:
- **→ CLAUDE.md rule**: The same mistake happens 2+ times, or it's a one-liner that applies everywhere
- **→ Skill gotcha**: The mistake is specific to a workflow that a skill covers (add to that skill's Gotchas section)
- **→ New skill**: The mistake reveals a missing workflow that needs its own multi-step procedure

### Phase 4: Proposed New Skills (Beyond the Existing 5)

Based on `prompts.md` patterns and `future_improvement.md` gaps:

#### `/fix-p1` — Fix Priority 1 Bugs

Trigger: "fix bug", "fix P1", "priority 1 fix"

Why: `future_improvement.md` has 5 P1 correctness bugs. Each one follows a pattern: read the bug description → find the file → understand the race/error → write the fix → write the test. A skill can encode that workflow.

Content:
1. Read `docs/dev/future_improvement.md` Priority 1 section
2. Ask which P1 bug to fix (or fix the next unfixed one)
3. Read the affected file(s)
4. Implement the fix following QUEST conventions
5. Write test that proves the bug is fixed
6. Update `future_improvement.md` to mark as done

#### `/build-test` — Build and Test Loop

Trigger: "build", "test", "run tests", "does it compile"

Why: The build/test cycle is 3 commands that everyone forgets the flags for. A command makes it one slash.

Content:
```bash
cmake --build build 2>&1 | tail -20
ctest --test-dir build --output-on-failure 2>&1 | tail -30
```

This is NOT a skill — it's a pure command (no knowledge needed, just execution).

## File Structure After Implementation

```
.claude/
├── settings.local.json
├── commands/
│   ├── new-op.md           # /new-op [name]
│   ├── new-node.md         # /new-node [name]
│   ├── verify.md           # /verify [operator]
│   ├── trace.md            # /trace [query]
│   ├── review-mem.md       # /review-mem [files...]
│   ├── fix-p1.md           # /fix-p1 [bug-id]
│   └── build-test.md       # /build-test
└── skills/
    ├── new-operator/SKILL.md        # + !command for enum listing
    ├── new-plan-node/SKILL.md       # + !command for existing nodes
    ├── verify-operator/SKILL.md     # + !command for git diff
    ├── trace-dataflow/SKILL.md      # (no changes)
    └── review-memory-safety/SKILL.md # + !command for changed files
```

## Implementation Order

1. **Create `.claude/commands/`** directory
2. **Write 5 commands** for existing skills (Phase 1) — quick, mechanical
3. **Add `!command` injections** to 4 skills (Phase 2) — verify the `!` syntax works first
4. **Update `tasks/lessons.md`** with the structured format (Phase 3)
5. **Create `/fix-p1` skill + command** (Phase 4) — tests the full pipeline
6. **Create `/build-test` command** (Phase 4) — pure command, no skill needed

## Verification

- [ ] Each command file exists and is valid markdown
- [ ] `/new-op` triggers the new-operator skill
- [ ] `/verify` runs adversarial tests via subagent
- [ ] `!command` syntax in skills injects dynamic output
- [ ] `tasks/lessons.md` has the structured format ready for entries
- [ ] Pipeline works end-to-end: lesson → rule → skill → command

## Key References

| File | Purpose |
|------|---------|
| `tutorial.md` | Workflow orchestration tutorial (Boris Cherny pattern) |
| `prompts.md` | 30 prompt patterns — seed for skill ideas |
| `CLAUDE.md` | Current rules and workflow |
| `docs/dev/future_improvement.md` | P1 bugs, missing features |
| `tasks/skills-proposal.md` | Original skill design rationale |
