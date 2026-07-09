# Pipeline Execution — Working Plan

Executing [docs/dev/pipeline-execution-plan.md](../docs/dev/pipeline-execution-plan.md).
**Process:** one stage = one commit; each stage gets a review summary BEFORE committing; user reviews and may request changes.

## Stage 0 — Sirius reading note ✅ (commit b789943)
- [x] `tasks/notes/pipelines.md`: how one batch crosses a pipeline break in Sirius (folded into Stage 1 commit)

## Stage 1 — Barrier vocabulary ✅ (commit b789943)
- [x] `BarrierType {Pipeline, Full}` on `EdgeConfig` (`kpair.hpp`); `Concatenating()` implies `Full`
- [x] `is_blocking_operator(OperatorType)` in `operator_type.hpp`
- [x] `Operator::IsBlocking()` virtual consulting the type classification
- [x] Tests: `tests/test_barrier_config.cpp` (14 tests) registered in `tests/CMakeLists.txt`
- [x] Build green: 487 passed / 0 failed / 1 skipped (pre-existing)
- [x] REVIEW GATE: user approved Stage 0; Stage 1 landed in the same commit, summary delivered post-hoc

## Stage 2 — Operator roles ✅ (commit 96d2673)
- [x] Hoist `ProcessBatch` to `Operator` (default passthrough); `SimpleOperator` redeclares it pure — legacy `run()` path untouched
- [x] `IsSource` (default: type classification) / `GetNextBatch` (EOS = engaged nullopt, not an error)
- [x] `IsSink` (default: `IsBlocking()`) / `Sink` (default: error) / `FinalizeSink` (default: OK no-op)
- [x] Test doubles: `VectorSourceOperator`, `AccumulatingSinkOperator` (mock blocking op with sink→source flip), `CountingStreamOperator`
- [x] `tests/test_operator_roles.cpp` (11 tests) incl. manual push-loop through all three roles
- [x] Build green: 498 passed / 0 failed / 1 skipped
- [x] REVIEW GATE: user approved ("LGTM") → committed 96d2673

## Stage 3 — Pipeline + MetaPipeline::Build ✅ (commit f9108cd + hardening follow-up)
**Design ref:** [`tasks/notes/session-handoff-pipeline.md`](notes/session-handoff-pipeline.md).
- [x] `pipeline.{hpp,cpp}`: operator list, counters, status lock, `RunTask(batch, start_index=1)` push loop
- [x] `meta_pipeline.{hpp,cpp}`: BuildFrom walk (memoized, children-before-parents)
- [x] Graph: store `EdgeConfig` per edge (`edge_configs_` map + `GetEdgeConfig`) — side map, `Edge` itself untouched
- [x] Graph: `GetTerminalOperators()` (pure query, no `FinalizeWorkflow()` mutation) as MetaPipeline::Build's walk starting points
- [x] DECIDED DEVIATION (kept): holders stay in `AddEdge` until Stage 6; pipelined path ignores intra-pipeline holders
- [x] Shape tests: linear->1; agg-in-middle->2 + FULL dep + agg in both; multi-input port-order->children before parent; explicit-Full-without-blocking split; Partitioned(2)->PIPELINE dep; memo/diamond (6 `MetaPipelineTest`)
- [x] `RunTask` push loop tests: streaming->sink, error mid-loop, single-op pipeline, non-blocking sink via real AddToOutput holder (4 `PipelineRunTaskTest`)
- [x] Build green: **508 passed / 0 failed / 1 skipped** (498 baseline + 10 new)
- [x] Self code review (via code-reviewer subagent) — see review log for findings + fixes
- [x] REVIEW GATE: user committed f9108cd, then requested an independent review pass
- [x] Hardening follow-up (P1–P4 from the independent review; see review log): 512 passed / 0 failed / 1 skipped

## Stage 4 — PipelineExecutor
- [ ] Worker pool, runnable set, completion events, claim-under-lock, teardown order
- [ ] `Graph::ExecutePipelined()`; old `Execute()` untouched
- [ ] Tests: FULL gating, PIPELINE overlap, completion race stress, error propagation, 2-worker bound
- [ ] REVIEW GATE

## Stage 5 — Parity + benchmark
- [ ] Parity old == new; spill-through-holder test; 10M-row benchmark recorded
- [ ] /verify-operator + /review-mem
- [ ] REVIEW GATE

## Stage 6 — Flip default
- [ ] `Graph::Execute()` delegates to pipelined path; docs updated
- [ ] REVIEW GATE

## Review log

### 2026-07-08 — Stage 3 hardening (review session, follow-up commit on top of f9108cd)
Independent review of the staged Stage 3 diff by a second session (the one that wrote the
handoff doc), requested by the user after committing f9108cd. Four prioritized findings
(P1–P4), all fixed in this follow-up; P5 note-level items intentionally left:
1. **P1 (real bug)** — `GetTerminalOperators()` returned nothing after `FinalizeWorkflow()`
   (sentinel `-1` edges made every terminal look non-terminal), so "legacy Execute, then
   Build" silently produced zero pipelines. The implementing session had documented this as a
   `@warning`; the review session fixed it instead: terminal detection now ignores edges with
   `target_operator_id < 0`. The obsolete "do NOT call FinalizeWorkflow first" warning on
   `MetaPipeline::Build` was replaced with "safe in either order".
2. **P2 (data-path hole)** — a blocking operator feeding a multi-input consumer
   (`agg >> join["input_a"]`) built a shape whose accumulated state nothing would ever re-emit
   into the join's port (no relay pipeline; the single-input path avoids this by making the
   blocking op the parent's source). `WalkMultiInputProducers` now rejects it with a clear
   `Status::Invalid` until relay pipelines exist (real-joins phase).
3. **P3 (latent race)** — `tasks_created_`/`tasks_completed_`/`finished_` were plain
   `size_t`/`bool`, relying on Stage 4 always holding `status_lock()`; Sirius completes tasks
   from worker-thread destructors without the lock. Now `std::atomic`, with the exact locking
   contract documented on `MarkTaskCreated`/`MarkTaskCompleted`.
4. **P4 (hang on malformed input)** — `BuildFrom` had no cycle protection; a cyclic graph hung
   (streaming cycle in the fuse walk) or overflowed the stack (blocking cycle in the
   recursion). Added an `in_progress_` set + per-walk visited set; both paths return
   `Invalid`, honoring the "error if malformed" contract.
Regression tests: +4 (`BuildStillFindsTerminalsAfterFinalizeWorkflow`,
`BlockingProducerIntoMultiInputConsumerIsRejected`, `StreamingCycleReturnsErrorInsteadOfHanging`,
`BlockingCycleReturnsErrorInsteadOfOverflowing`). Suite: **512 passed / 0 failed / 1 skipped**.

### 2026-07-08 — Stage 3 (implementing session; committed f9108cd)
- New: `pipeline.{hpp,cpp}`, `meta_pipeline.{hpp,cpp}`, `tests/test_meta_pipeline.cpp` (10 tests).
  Modified: `graph.{hpp,cpp}` (+`edge_configs_`/`GetEdgeConfig`, +`GetTerminalOperators`),
  `CMakeLists.txt`, `tests/CMakeLists.txt`. 508 passed / 0 failed / 1 skipped.
- `BuildFrom` matches the handoff pseudocode exactly (verified by hand-tracing all 6 shape
  cases against both the algorithm and the passing tests): blocking-check before edge-config
  check; single vs multi-input dispatch; port-name-sorted multi-input children; memoized
  diamond; children always pushed before the parent that depends on them.
- Code-reviewed via the code-reviewer subagent. Two real findings, both addressed with
  documentation (not behavior changes, since neither is in Stage 3's own execution scope):
  1. `Pipeline::PublishOutput`'s `AddToOutput()` call only targets the default (operator-id)
     port. A pipeline whose sink ends at a *partitioned* edge (`num_partitions > 1`) would
     publish to the wrong port and get `Status::Invalid` back — a clean failure, not silent
     data loss, but real. Nothing in Stage 3 executes a partitioned pipeline yet (shape-only
     test), so documented as a `@note` on `PublishOutput` with the fix deferred to Stage 4
     (thread the edge's actual output port through once an executor runs partitioned work).
  2. `GetTerminalOperators()` returns nothing if `FinalizeWorkflow()` already ran on the same
     graph (its sentinel edges make every real terminal's outgoing set non-empty). Not
     triggered today; documented as a `@warning` on both `GetTerminalOperators()` and
     `MetaPipeline::Build()` so a future `ExecutePipelined()` doesn't reuse `Execute()`'s prep
     step and silently build zero pipelines.
- Also fixed two clang-tidy warnings in the new `src/` code (short parameter name in
  `PrependOperator`; `PublishOutput` made `const` — it never mutates `Pipeline` state).
- No regressions: all 498 pre-existing tests still green.

### 2026-07-08 — Stage 0+1 (`b789943`)
- 7 files, +378/−209. Pure vocabulary, no execution path reads `barrier` yet.
- Notable decision: `EdgeConfig::Concatenating()` now implies `BarrierType::Full`.
- Key Sirius finding captured in the note: the FULL barrier is checked in the
  consumer's `get_next_task_hint()` — barriers gate task creation, never a
  worker thread. `RunTask` will carry a `start_index` for future OOM resume.
- Process note: Stage 1 code was committed together with Stage 0 (tree was
  fully staged); review summary delivered after the commit instead of before.
