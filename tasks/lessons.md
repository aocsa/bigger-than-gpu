# Lessons Learned

Mistakes logged here get promoted to rules (CLAUDE.md) or skill gotchas when they repeat.

## Format

```
## Lesson #{N}: {title}
**Date**: YYYY-MM-DD
**What happened**: {description of the mistake}
**Root cause**: {why it happened}
**Rule**: {the prevention rule — actionable, specific}
**Promoted to**: none | CLAUDE.md | skill:{skill-name}
```

## Promotion Criteria

- **-> CLAUDE.md rule**: Same mistake happens 2+ times, or it's a one-liner that applies everywhere
- **-> Skill gotcha**: Mistake is specific to a workflow a skill covers (add to that skill's Gotchas section)
- **-> New skill**: Mistake reveals a missing workflow that needs its own multi-step procedure

---

<!-- Lessons go below this line -->

## Lesson #1: A benchmark of engine overhead must scale the thing the engines differ in
**Date**: 2026-07-09
**What happened**: The Stage 5 benchmark (10M rows, 100 batches of 100k rows, passthrough
operators) measured 0 ms vs 0 ms — useless. Both engines moved 100 shared_ptrs; the row count
was irrelevant because no operator touched rows.
**Root cause**: The plan said "10M rows" and I anchored on rows, but with passthrough operators
the engines differ only in per-BATCH overhead (queue handoffs + thread wakeups vs a fused call
chain). 100 batches of overhead is sub-millisecond.
**Rule**: Before writing a benchmark, name the axis the variants actually differ on and scale
THAT axis (here: 10k batches × 1k rows made it 21 ms vs 4 ms). If the first run shows 0 ms or
identical numbers, treat the benchmark as broken, not the difference as zero.
**Promoted to**: none

## Lesson #2: Flipping a default execution path requires auditing every caller's CONTRACT, not just call sites
**Date**: 2026-07-09
**What happened**: Making Graph::Execute() delegate to the pipelined engine would silently break
all run()-based operators (test_graph mocks, examples) — they implement the legacy contract
only, and the pipelined engine never calls run().
**Root cause**: The two engines share a graph API but demand different operator contracts
(run() vs ProcessBatch/GetNextBatch/Sink roles); a call-site grep looks clean while the
behavior changes underneath.
**Rule**: When flipping a default that dispatches to user-implemented interfaces, enumerate the
implementations (not the callers) and route unmigrated ones through an explicit escape hatch
(ExecuteLegacy()) with a deletion plan — never let them ride the new default.
**Promoted to**: none
