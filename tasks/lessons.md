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

## Lesson #4: Distinguish card placement from port-level switch wiring
**Date**: 2026-09-17
**What happened**: The initial Zeno diagram followed the supplied card-to-switch description.
The user's subsequent LLDP findings for Zeno-01 and Zeno-02 showed that each card connects
to both switches: both cards' f0 ports go to ToR-2, and both f1 ports go to ToR-1.
**Root cause**: An assumed breakout grouping was illustrated before port-level evidence
was available; each switch breakout actually feeds ports on two different cards.
**Rule**: Label unverified wiring as assumed. When port-level evidence arrives, map host,
card/function, link speed, switch identity, and breakout grouping explicitly. Do not extend
observations from verified hosts to unexamined hosts or equate link capacity with measured
NIXL throughput. The shown evidence does not verify Zeno-03.
**Promoted to**: none

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
**Follow-up (2026-08-01)**: ExecuteLegacy() and all run()-based operators were subsequently
deleted once every caller migrated to pipeline roles. The escape hatch did its job.
**Promoted to**: none

## Lesson #3: Redraw third-party architecture from the project's own figures, not from recall
**Date**: 2026-08-11
**What happened**: I drew a StarRocks architecture diagram from background knowledge. It merged
shared-nothing (BE + local tablets) and shared-data (CN + object storage) into ONE cluster with
a BE and a CN exchanging data — a configuration that does not exist. It also dropped the internal
components the official figure is built around (Catalog Manager / Query Coordinator in every FE;
Execution Engine / Storage Engine in every BE) and showed only one FE role instead of
Leader/Follower/Observer.
**Root cause**: The diagram looked plausible and self-consistent, so nothing prompted me to check
it against a source. Plausibility is not accuracy for someone else's system.
**Rule**: Before drawing another project's architecture, fetch that project's own diagram
(README, docs site, repo images) and diff my node set against it. Deployment MODES are mutually
exclusive — never draw two modes as one cluster; render them as separate figures the way the
upstream docs do.
**Promoted to**: none
