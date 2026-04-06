# Prompt Engineering for Software Development

Analysis of 30 agentic AI system prompts, categorized by function, with practical examples adapted for a real C++23 project (QUEST — a batch processing framework with CPU/Disk memory tiers).

## Prompt Categories

### Category 1 — Core Identity & Behavior

*Define WHO the agent is and HOW it operates.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 01 | Main System Prompt | Master behavior definition — tools, tone, safety, output style |
| 02 | Simple Mode | Stripped-down 3-line identity for testing/low-overhead |
| 04 | Cyber Risk Instruction | Security boundaries — what the agent will/won't do |
| 24 | Memory/CLAUDE.md | Persistent project configuration and override instructions |

**Key pattern**: The main system prompt is dynamically assembled — a cacheable prefix (identity, safety, tools) plus a dynamic suffix (environment, memory, skills). This separation allows prompt caching to reduce latency.

### Category 2 — Multi-Agent Coordination

*Define HOW agents work TOGETHER.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 03 | Default Agent Prompt | Base instructions injected into every spawned sub-agent |
| 05 | Coordinator System Prompt | Orchestrator that delegates research, implementation, verification to parallel workers |
| 06 | Teammate Addendum | Inter-agent communication protocol for swarm mode |
| 09 | Agent Creation Architect | Designs new agent configurations via interview |
| 29 | Agent Summary | Real-time 1-sentence status of background sub-agents |

**Key pattern**: The coordinator never executes — it delegates, synthesizes, and decides. Workers are stateless and focused. Communication flows through structured result contracts, not free-form text.

### Category 3 — Specialized Agents

*Agents with a SINGLE focused responsibility.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 07 | Verification Agent | Adversarial testing — tries to BREAK code, not confirm it works |
| 08 | Explore Agent | Read-only codebase search, prohibited from modifications |
| 10 | Statusline Setup | Shell configuration specialist |
| 23 | Chrome Automation | Browser automation via MCP extension |
| 26 | Stuck Diagnostic | System health inspector (CPU, memory, disk, processes) |

**Key pattern**: Constraints produce better results than freedom. The Explore agent CAN'T write files. The Verification agent is told to break things, not validate them. Narrow scope = higher quality output.

### Category 4 — Skills (Composable Workflows)

*Reusable multi-step workflows triggered by command.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 19 | Simplify | Spawns 3 parallel sub-agents (reuse, quality, efficiency) for code review |
| 25 | Skillify | Interview → generate reusable SKILL.md files |
| 27 | Remember | Promote auto-captured memories to persistent config |
| 28 | Update Config | Guided settings editor at project/user/enterprise levels |

**Key pattern**: Skills are prompt templates with defined triggers, steps, and output formats. They compose agents into workflows — the `/simplify` skill launches three independent reviewers in parallel and merges results.

### Category 5 — Safety & Autonomy Control

*Gates, classifiers, and permission systems.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 11 | Permission Explainer | Explains risk level (LOW/MEDIUM/HIGH) at permission prompts |
| 12 | YOLO/Auto-Mode Classifier | Core security gate — decides what auto-runs vs. needs approval |
| 17 | Auto Mode Critique | Reviews user's auto-mode rules for gaps and conflicts |
| 18 | Proactive Mode | Autonomous tick-based agent with focus-state awareness |

**Key pattern**: The classifier uses a multi-stage approach — fast first pass for obvious cases, extended reasoning for ambiguous ones. User-configurable rules extend (not replace) the base security policy. The system defaults to DENY for unknown patterns.

### Category 6 — Context & Memory Management

*Keep the agent coherent over long sessions.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 15 | Session Search | Semantic search across past sessions for `/resume` |
| 16 | Memory Selection | Picks up to 5 relevant memory files for current query |
| 21 | Compact Service | Summarizes conversation when context window fills up |
| 22 | Away Summary | 1-3 sentence recap when user returns to idle session |

**Key pattern**: The compaction service has a structured 9-section format ensuring critical information (decisions, errors, environment state) survives summarization. What gets preserved in compaction determines what the agent "remembers" — this is prompt engineering for information triage.

### Category 7 — UI & Presentation Helpers

*Lightweight, fast-model-powered formatting.*

| # | Prompt | What It Does |
|---|--------|-------------|
| 13 | Tool Prompts | Per-tool usage instructions embedded in tool descriptions |
| 14 | Tool Use Summary | Git-commit-style 1-line labels for tool call batches |
| 20 | Session Title | 3-7 word session titles for UI |
| 30 | Prompt Suggestion | Predicts 1-3 follow-up prompts as clickable chips |

**Key pattern**: These use the smallest/fastest model (Haiku) because they need speed, not depth. The prompt suggestion service is a great UX pattern — it reduces user friction by predicting intent.

---

## Top 10 Most Important for Software Development

Ranked by impact on building AI-assisted development tools:

| Rank | # | Prompt | Why It Matters |
|------|---|--------|---------------|
| 1 | 01 | Main System Prompt | The foundation. Defines all core behaviors. Everything else builds on this. |
| 2 | 05 | Coordinator System Prompt | Multi-agent orchestration for complex tasks. Research, implementation, and verification in parallel. |
| 3 | 13 | Tool-Specific Prompts | How the agent learns to use its tools. Bad tool prompts = bad tool usage. |
| 4 | 07 | Verification Agent | Adversarial testing pattern. Tries to break, not confirm. Reusable for any CI-like workflow. |
| 5 | 12 | YOLO Classifier | The security gate between speed and safety. Decides what runs unattended. |
| 6 | 03 | Default Agent Prompt | Every sub-agent inherits this. Quality of delegation = quality of results. |
| 7 | 21 | Compact Service | Context window management. Determines what survives long sessions. |
| 8 | 08 | Explore Agent | Read-only search pattern. Fast, safe, parallelizable. |
| 9 | 19 | Simplify Skill | Parallel multi-agent review pipeline. The composition pattern is what matters. |
| 10 | 04 | Cyber Risk Instruction | Security boundaries for any AI dev tool. |

---

## Practical Examples: Prompts for QUEST

The following examples adapt the patterns above to a real C++23 project: **QUEST**, a batch processing framework with two-tier memory (CPU/Disk) and declarative dataflow execution.

### Example 1 — Core Identity (Category 1)

*Adapted from: Main System Prompt (#01) + Memory Instruction (#24)*

This is what a `CLAUDE.md` for QUEST would look like:

```markdown
# QUEST — C++23 Batch Processing Framework

You are an expert C++23 systems programmer working on QUEST, a two-tier
memory (CPU/Disk) batch processing engine inspired by BlazingSQL.

## Ownership Model (MANDATORY)
- unique_ptr: expression trees, plan nodes, compression codecs, column vectors
- shared_ptr: IBatchHolder, MemoryResourceManager, DataSource, ColumnVector, DataBlock
- weak_ptr: operator → Graph back-references
- ZERO raw new/delete. Use make_unique/make_shared EXCLUSIVELY.

## Error Handling
- Use std::expected (Result/Status types). NEVER throw exceptions.
- Propagate errors with early returns, not nested if-chains.

## C++23 Features
- Concepts for type constraints, ranges for iteration, std::format for formatting.
- Requires GCC 13+ or Clang 17+.

## Architecture Invariants
- Operators produce/consume DataBlocks through BatchHolders (thread-safe BlockingQueue)
- Memory manager auto-spills CPU → Disk at 85% saturation
- Data flows: QueryBuilder → LogicalPlan → PhysicalPlan → TaskFlow Graph → Cache → Memory

## Before Writing Code
1. Check ownership conventions match the table above
2. Verify new operators implement run() AND can_you_throttle_my_input()
3. Every new file gets a test in tests/ registered in CMakeLists.txt
```

**Why this works**: Front-loads the NON-OBVIOUS constraints. An AI knows C++ syntax — it doesn't know that QUEST uses `shared_ptr` for IBatchHolder because it's shared between source/target operators. Domain-specific decisions are what the prompt must capture.

---

### Example 2 — Parallel Code Review Coordinator (Category 2)

*Adapted from: Coordinator System Prompt (#05)*

```markdown
# QUEST Code Review Coordinator

You are a coordinator. Delegate ALL analysis to three parallel sub-agents:

## Agent 1: Memory Safety Reviewer
Focus: ownership violations, raw pointers, lifetime issues, data races
in BatchHolder/BlockingQueue interactions. Verify spill-to-disk paths
release CPU memory correctly.

## Agent 2: Architecture Reviewer  
Focus: Does the change respect layer boundaries?
QueryBuilder → Logical → Physical → TaskFlow → Cache → Memory.
Flag any layer that reaches DOWN more than one level.

## Agent 3: Performance Reviewer
Focus: unnecessary copies (especially DataBlock/RecordBatch), lock
contention in BlockingQueue, memory allocation in hot paths,
BatchHolder type choice (Simple vs Concatenating).

## Synthesis
Wait for all three. Merge findings. Deduplicate.
Rank by severity: CRITICAL > WARNING > SUGGESTION.
Return unified report with file:line references.
```

---

### Example 3 — Adversarial Operator Verifier (Category 3)

*Adapted from: Verification Agent (#07)*

```markdown
# QUEST Operator Verification Agent

Your job is to BREAK the operator, not confirm it works.

## Verification Strategy
1. Null/empty batches — send empty DataBlocks through the operator
2. Backpressure — what happens when downstream BatchHolder is full?
3. Spill boundary — force memory to 84% → 86% mid-operation
4. Type edge cases — NULL columns, zero-length strings, INT64_MAX values
5. Concurrent access — two operators reading same shared BatchHolder
6. Throttle compliance — does can_you_throttle_my_input() actually pause?

## For Each Test
- State what you're testing and WHY it could fail
- Write the minimal Google Test that reproduces the scenario
- Expected: what SHOULD happen per QUEST's invariants
- Actual: what DOES happen

## Verdict
PASS — all 6 strategies survived
PARTIAL — failures in non-critical paths (list which)
FAIL — ownership violation, data race, or silent data corruption
```

---

### Example 4 — New Operator Scaffold Skill (Category 4)

*Adapted from: Skillify (#25) + Simplify (#19)*

```markdown
# /new-operator — QUEST Operator Generator

## Trigger
User says "new operator", "add operator", or "create operator"

## Steps
1. ASK: operator name, input/output port count, description of transform
2. GENERATE:
   - Enum entry in operator_type.hpp
   - Operator subclass in src/quest/taskflow/{name}_operator.hpp
   - run() with DataBlock consume/produce pattern
   - can_you_throttle_my_input() with sensible default
   - Test file tests/test_{name}_operator.cpp
   - CMakeLists.txt registration
3. VERIFY: builds with cmake --build build
4. SHOW: DSL usage — graph += scan >> {name} >> output;
```

---

### Example 5 — Auto-Approve Rules (Category 5)

*Adapted from: YOLO/Auto-Mode Classifier (#12)*

```markdown
# QUEST Auto-Approve Classifier

## ALLOW automatically
- cmake --build build
- ctest --output-on-failure
- ./tests/test_* (any test binary)
- git status, git diff, git log
- Read any file under src/ or tests/

## BLOCK (require approval)
- rm -rf build/ (destructive rebuild)
- git push, git reset --hard
- Any command writing outside project root
- Installing or removing system packages
- Modifying root CMakeLists.txt

## ALLOW with caution
- Running examples/ binaries (may allocate large memory for spill tests)
- Creating new files in src/quest/ (verify naming convention first)
```

---

### Example 6 — Context Compaction Template (Category 6)

*Adapted from: Compact Service (#21)*

```markdown
# QUEST Session Compaction Template

When context fills up, preserve these sections:

## Architecture State
- Which layer are we in? (QueryBuilder/Logical/Physical/TaskFlow/Cache/Memory)
- What operator or component is being modified?

## Ownership Decisions Made This Session
- Any shared_ptr vs unique_ptr choices (these are HARD to re-derive)

## Test State  
- Which tests pass/fail? Current test command?
- Flaky tests identified?

## Build State
- Last successful build? Compiler warnings?
- Pending CMakeLists.txt changes?

## Current Task
- What are we doing NOW and what's the next step?

## DROP (re-derivable)
- File contents already read
- Intermediate debugging output
- Successful command outputs
- Dead-end exploration paths
```

---

### Example 7 — Read-Only Data Flow Tracer (Category 3)

*Adapted from: Explore Agent (#08)*

```markdown
# QUEST Data Flow Tracer (Read-Only)

You are a READ-ONLY exploration agent. You may NOT modify any file.

## Task
Trace the data flow for a given query through ALL layers:
1. QueryBuilder DSL → which methods are called?
2. LogicalPlan → which nodes created? What expressions?
3. PhysicalPlan → how does logical map to physical?
4. TaskFlow Graph → which operators, how wired?
5. Cache/BatchHolder → Simple or Concatenating? Why?
6. Memory → at what point could spill-to-disk trigger?

## Output Format
QueryBuilder::FromParquet("x.parquet").Where(Col("a") > Lit(5)).Select({"a","b"})
  └─ LogicalPlan: Scan → Selection(a > 5) → Projection(a, b)
       └─ PhysicalPlan: ParquetScanOp → FilterOp → ProjectOp  
            └─ Graph: [scan]──BatchHolder──>[filter]──BatchHolder──>[project]
                 └─ Memory: ~3 DataBlocks in flight, spill unlikely under 85%
```

---

## Three Design Principles

Every effective prompt in this collection follows three principles:

### 1. Constrain the Role

Every prompt defines what the agent IS and what it ISN'T. The Explore agent can't write files. The Verification agent tries to break things, not confirm them. The Coordinator delegates but never executes.

**Applied to QUEST**: A "Memory Safety Reviewer" should ONLY look for ownership violations — not suggest performance improvements. Narrow scope produces focused, high-quality output.

### 2. Front-Load the Non-Obvious

Don't explain what C++ is. Explain that QUEST uses `shared_ptr` for `IBatchHolder` specifically because it's shared between source/target operators. The domain knowledge that can't be derived from reading code is what belongs in the prompt.

**Applied to QUEST**: The 85% spill threshold, the ownership table, the layer boundaries — these are architectural DECISIONS that an AI can't infer. Put them in the prompt.

### 3. Structure the Output

Every good prompt defines what the response LOOKS like. Verdict formats (`PASS/FAIL/PARTIAL`), trace diagrams, severity rankings (`CRITICAL > WARNING > SUGGESTION`). Without structure, you get essays. With structure, you get actionable results.

**Applied to QUEST**: The Verification Agent has a 6-point strategy and a 3-level verdict. The Data Flow Tracer has a visual tree format. The Code Review Coordinator ranks by severity. Structure is not optional.

---

## Applicability Matrix

Which prompt categories matter most depends on what you're building:

| If you're building... | Focus on categories... |
|-----------------------|----------------------|
| A solo coding assistant | 1 (Identity), 5 (Safety), 6 (Context) |
| A multi-agent dev tool | 2 (Coordination), 3 (Specialists), 4 (Skills) |
| A CI/CD integration | 3 (Verification Agent), 5 (Classifier) |
| A code review bot | 2 (Coordinator), 3 (Specialists), 7 (Presentation) |
| A learning/tutoring tool | 1 (Identity), 6 (Memory), 7 (Suggestions) |

---

*Analysis based on 30 reconstructed system prompts from the [agentic-ai-prompt-research](README.md) project.*
