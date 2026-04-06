# CLAUDE.md Redesign — Elegant Architecture

## Problem

The current CLAUDE.md (~100 lines) duplicates information already documented in:
- `docs/dev/ARCHITECTURE.md` (718 lines) — full architecture, class diagrams, design patterns
- `docs/dev/CONTRIBUTING.md` (178 lines) — build, test, directory map, debugging order
- `docs/dev/query_builder_refactor.md` — builder split documentation
- `docs/dev/future_improvement.md` — known bugs, roadmap

CLAUDE.md is trying to be a reference document AND an instruction set. It should be
**only instructions** — rules that change behavior. Everything else is documentation that
Claude can read when it needs context.

## Principle

> CLAUDE.md tells Claude **how to work**. Docs tell Claude **what the project is**.

If Claude can derive it by reading a file → don't put it in CLAUDE.md.
If it's a rule that changes behavior → it MUST be in CLAUDE.md.

## Current Structure (what's wrong)

```
CLAUDE.md
├── Build & Test              ← duplicated from CONTRIBUTING.md
├── Architecture              ← duplicated from ARCHITECTURE.md
├── Directory table           ← duplicated from CONTRIBUTING.md
├── Ownership Model           ← RULE (keep)
├── Error Handling            ← RULE (keep)
├── Code Format               ← RULE (keep)
├── C++23 Features            ← reference info, not a rule
├── Architecture Invariants   ← mix of rules and reference
├── Before Writing Code       ← RULE (keep)
├── Library Target            ← reference info
├── Debugging Trace Order     ← duplicated from CONTRIBUTING.md
├── Workflow sections          ← RULES (keep)
├── Task Management           ← RULES (keep)
└── Core Principles           ← RULES (keep)
```

## Proposed Structure

Three clean sections. No reference material. Under 80 lines.

```
CLAUDE.md
├── 1. IDENTITY (2 lines)     — what is this project, one sentence
├── 2. MANDATORY RULES        — things that change code behavior
│   ├── Ownership Model
│   ├── Error Handling
│   ├── Code Format
│   └── Before Writing Code
├── 3. WORKFLOW                — things that change work process
│   ├── Plan Mode Default
│   ├── Subagent Strategy
│   ├── Self-Improvement Loop
│   ├── Verification Before Done
│   ├── Demand Elegance
│   ├── Autonomous Bug Fixing
│   ├── Task Management
│   └── Core Principles
└── 4. CONTEXT (3 lines)      — pointers to docs, not content
```

## What Gets Removed (and why)

| Section | Why remove | Where it lives |
|---------|-----------|----------------|
| Build & Test commands | Claude can read CONTRIBUTING.md | `docs/dev/CONTRIBUTING.md` |
| Architecture flow diagram | Claude can read ARCHITECTURE.md | `docs/dev/ARCHITECTURE.md` |
| Directory table | Claude can read CONTRIBUTING.md | `docs/dev/CONTRIBUTING.md` |
| C++23 Features description | Informational, not behavioral | `docs/dev/ARCHITECTURE.md` |
| Architecture Invariants (partial) | The BatchHolder types are reference | `docs/dev/ARCHITECTURE.md` |
| Library Target | Reference info | `README.md` |
| Debugging Trace Order | Already in CONTRIBUTING.md | `docs/dev/CONTRIBUTING.md` |

## What Gets KEPT (and why)

| Section | Why keep |
|---------|---------|
| Ownership Model | MANDATORY rule — wrong choice = memory bugs |
| Error Handling | MANDATORY rule — exceptions vs expected changes behavior |
| Code Format | MANDATORY rule — formatting convention |
| Before Writing Code | Checklist that prevents common mistakes |
| All Workflow sections | Process rules that change how Claude operates |
| Task Management | Operational protocol |
| Core Principles | Behavioral guardrails |

## Key Insight: Architecture Invariants

The current "Architecture Invariants" section mixes rules and reference:
- "Memory manager auto-spills at 85%" → reference (Claude reads the code)
- "QueryBuilder methods are rvalue-qualified" → reference (it's in the API)
- "ZERO raw new/delete" → RULE (keep in ownership model)

The rvalue-qualified note is worth keeping as a gotcha, but it belongs in
"Before Writing Code" as a checklist item, not as an architecture description.

## Implementation

Single file rewrite. ~75 lines. Every line is either a rule or a pointer.
