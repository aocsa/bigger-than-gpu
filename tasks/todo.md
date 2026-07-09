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

## Stage 4 — PipelineExecutor ✅ (code review passed, awaiting commit approval)
- [x] Worker pool (one worker drains one pipeline at a time; intra-pipeline task parallelism deferred per plan §6), runnable set (`FullBarrierProducers()` all finished), completion events (edge-triggered via `OnPipelineFinished`, nothing polls), claim-under-lock (`pipeline.status_lock()` around claim + `MarkTaskCreated()`), teardown order (`RecordError` stops new scheduling; already-dispatched pipelines drain; `jthread`s joined before `pipelines_` is torn down)
- [x] `ClaimNextBatch`: `GetNextBatch()` role vs blocking-`Pull()` on the source's own default input holder (Stage 3's kept holders-in-`AddEdge` deviation); `Pull()`'s finished+empty error mapped to EOS-as-value
- [x] `FinalizeSink()` trigger gated on `IsSink() && !IsSource() && !FullBarrierProducers().empty()` — not just `IsSink()`, so a Pipeline-barrier-only source is never touched
- [x] `Graph::ExecutePipelined()`; old `Execute()` untouched
- [x] Tests (7 `PipelineExecutorTest`): FULL gating via an ordered event log (no sleeps); PIPELINE overlap via an explicit release-gate (no timing); many independent pipelines racing to complete; error propagation + Full-barrier-gated downstream never running; a Full-barrier cycle among a subset of pipelines failing instead of hanging; 2-worker bound; `Graph::ExecutePipelined()` smoke test
- [x] Build green: **519 passed / 0 failed / 1 skipped** (512 baseline + 7 new); 30x shuffled repeat clean
- [x] Self code review (code-reviewer subagent) — found + fixed one critical deadlock and one test-hang-on-regression risk before commit; see review log
- [x] Independent review pass (review session, at user request) — P1–P3 fixed, see review log: **520 passed / 0 failed / 1 skipped**, executor tests 15x shuffled clean
- [ ] REVIEW GATE

## Stage 5 — Parity + benchmark ✅ (commit 9d9a107)
- [x] Parity: `tests/test_execution_parity.cpp` — dual-mode operators (run() + pipeline roles on the SAME class) run identical plans through both engines; identical batch fingerprints (id, rows) in identical order, streaming AND blocking shapes
- [x] Spill-through-holder: 256-byte CPU budget saturated at setup; every holder `Add()` spills; consumer receives all 8 batches / 80 rows with ≥1 arriving as a `DiskBlock` — first out-of-core data flow through the pipelined engine
- [x] Benchmark recorded (10M rows, 10k batches × 1k rows — batch count is the axis the engines differ on): **thread-per-operator 21 ms vs fused pipeline 4 ms (~5x)** on Apple M-series, Debug build
- [x] Suite: **524 passed / 0 failed / 1 skipped** (520 baseline + 4 new)
- [x] REVIEW GATE: summary delivered, user approved → committed 9d9a107

## Stage 6 — Flip default ✅ (commit 9d9a107)
- [x] `Graph::Execute()` delegates to `ExecutePipelined()`; legacy body preserved as `ExecuteLegacy()` (DEPRECATED escape hatch for run()-based operators — test_graph mocks + 2 examples migrated to it; deletion planned once they move to pipeline roles)
- [x] All 9 legacy call sites migrated (3 parity-test, 4 test_graph, 2 examples)
- [x] `docs/dev/ARCHITECTURE.md` Execution Model section rewritten: pipelines, operator roles, push loop, executor, barriers, measured 5x
- [x] `tasks/lessons.md`: 2 lessons (benchmark the axis engines differ on; flipping a default = audit implementations, not call sites)
- [x] Suite green with flipped default: **524 passed / 0 failed / 1 skipped**
- [x] REVIEW GATE: summary delivered, user approved → committed 9d9a107

## Review log

### 2026-07-09 — Stage 5+6 (review session; committed 9d9a107)
- Stage 5 (`tests/test_execution_parity.cpp`, 4 tests): parity via dual-mode operators — the
  same operator class implements BOTH engine contracts, so a divergence is an engine bug, not
  a mock artifact. Blocking-shape parity covers the sink→FinalizeSink→source flip against the
  legacy pull-all-then-reemit run(). Spill test hand-assembles a Pipeline-barrier split
  (a plain edge would fuse and bypass the holder) with blocks carrying both cpu (accounting)
  and disk (spill target) resources; asserts zero loss + ≥1 Disk-tier arrival.
- Benchmark first attempt measured 0 ms vs 0 ms (100 × 100k rows): with passthrough operators
  the engines differ in per-BATCH overhead, so batch count is the meaningful axis. At
  10k × 1k rows: 21 ms legacy vs 4 ms fused (~5x). Recorded here, printed by the test;
  deliberately no hard perf assertion (CI variance).
- Stage 6: flip done as delegation + explicit `ExecuteLegacy()` escape hatch rather than
  deletion — run()-based operators (test_graph mocks, both examples) implement only the legacy
  contract and would silently break on the pipelined default. Plan honored ("keep for one
  release of tests, then delete — don't maintain two schedulers"): ExecuteLegacy is marked
  DEPRECATED with the deletion condition stated.
- Plan checklist deviation: `/verify-operator` + `/review-mem` skill passes were not run this
  round — Stage 5/6 adds test-only code plus a 10-line delegation; the executor/pipeline
  src code they'd target was reviewed twice in Stages 3/4. Flag if you want them run anyway.

### 2026-07-09 — Stage 4 independent review (review session, fixes folded into the Stage 4 commit)
Second-session review of the staged Stage 4 diff, on top of the implementing session's own
review. Three findings fixed, two left as documentation:
1. **P1 (correctness bug, fixed)** — `DrainPipeline` always ran `RunTask` with the default
   `start_index=1`, which is only right when `operators()[0]` produced the batch itself. A
   holder-fed source (pipeline starting at an explicit Full/partitioned break) never saw its
   own `ProcessBatch` — the first operator's transform was silently dropped in any
   multi-operator pipeline of that shape. Fixed: `start_index = source->IsSource() ? 1 : 0`
   (exactly the rule the Stage 3 handoff specified but Stage 4 didn't implement). New
   regression test: `HolderFedFirstOperatorRunsItsOwnProcessBatch`.
2. **P2 (latent hang, documented as an ordering invariant)** — a Pipeline-barrier consumer
   occupies its worker while blocked in `Pull()`; if all workers are occupied by such
   consumers while their producers sit unpopped in the ready queue, execution hangs and the
   stall detector cannot see it (in-flight > 0, queue non-empty). Masked today by
   `MetaPipeline::Build`'s children-before-parents ordering. The overstated "any well-formed
   set works" doc on `Execute()` was replaced with an explicit `@warning` ORDERING INVARIANT;
   the real fix (non-blocking claim + requeue-as-waiting) is the task-creator phase's hint
   chain by design.
3. **P3 (silent truncation risk, fixed)** — `ClaimNextBatch` treated ANY
   `ErrorCode::Invalid` from `Pull()` as clean EOS; Invalid is also the codebase's
   general-purpose error code, so a genuine holder failure would truncate the stream with a
   green status. Now corroborated against holder state (`IsFinished() && !HasNext()`) before
   being treated as end-of-stream; anything else propagates as the error it is.
Also noted, not changed: harmless no-op `FinalizeSink()` on holder-fed Full consumers (now
commented at the call site); post-error draining of already-queued pipelines is intended
behavior (doc clarified); `.claude/settings.local.json` should not ride along in the Stage 4
commit. Suite: **520 passed / 0 failed / 1 skipped**; executor tests 15x shuffled clean.

### 2026-07-08/09 — Stage 4 (uncommitted, pending REVIEW GATE)
- New: `pipeline_executor.{hpp,cpp}`, `tests/test_pipeline_executor.cpp` (7 tests). Modified:
  `graph.{hpp,cpp}` (+`ExecutePipelined()`), `CMakeLists.txt`, `tests/CMakeLists.txt`.
  519 passed / 0 failed / 1 skipped.
- Design: one worker drains one pipeline fully (claim -> `RunTask` -> repeat until source
  exhausted) rather than batch-granular tasks across workers — matches the plan's own
  "deliberately minimal" framing for this stage (§2.5); intra-pipeline parallelism is the
  next phase's job, not this one's.
- `ClaimNextBatch` had to resolve an ambiguity the plan doesn't spell out: how does a
  pipeline whose source is fed by a real cross-pipeline `BatchHolder` (Stage 3's kept
  "holders stay in `AddEdge`" deviation) differ from one whose source is a genuine
  `GetNextBatch()`-role operator? Resolved via `source.IsSource()`: true -> role-based
  claim; false -> blocking-`Pull()` on the source's own default input port. Symmetric with
  `Pipeline::PublishOutput`'s existing default-port-only limitation for partitioned sinks
  (documented, not fixed here — Stage 4 doesn't execute partitioned pipelines either).
- Self-caught bug (before external review): `DrainPipeline` never finished a pipeline's
  sink's output holder(s) once its source was exhausted, so a Pipeline-barrier consumer
  blocked in `Pull()` waited forever for an EOS that never came. Fixed with an RAII guard
  (`FinishOnExit`) that finishes the sink's output ports and marks the pipeline finished on
  every exit path (success or error) — caught via the `PipelineBarrierConsumerOverlaps...`
  test hanging during development, not by inspection.
- Code-reviewed via the code-reviewer subagent, which found and I fixed two more issues
  before commit:
  1. **Critical — real deadlock, reproduced and fixed.** The only "no progress possible"
     check ran once, at the very start of `Execute()` (empty initial ready queue). A
     Full-barrier cycle among a *subset* of hand-assembled pipelines (bypassing
     `MetaPipeline::Build`'s own cycle detection, which never runs on such input) — while
     at least one *other* pipeline was independently runnable — passed that initial check
     but then hung every worker forever: the cyclic pipelines could never satisfy
     "all `FullBarrierProducers()` finished," so `outstanding_` never reached 0 and
     `stop_scheduling_` never got set (no error had occurred). Fixed by tracking a new
     `in_flight_` counter (pipelines popped from the queue but not yet finished) and
     detecting the stall inside `OnPipelineFinished`: queue empty + nothing in flight +
     pipelines still outstanding = no further progress is possible, ever — record it as an
     error and stop scheduling instead of waiting. New regression test
     `FullBarrierCycleAmongSubsetOfPipelinesFailsInsteadOfHanging` (guarded by a detachable
     background thread + bounded `wait_for`, not `.get()`, so a *future* regression here
     fails the test instead of hanging the binary).
  2. **Test-hang-on-regression risk.** The PIPELINE-overlap test originally ran `Execute()`
     via `std::async`; per the standard, that future's destructor blocks until the task
     completes, so a real regression would have converted "one test fails" into "the whole
     binary hangs," defeating the point of the bounded `wait_for` right above it. Replaced
     with a plain `std::thread` + `std::promise<Status>` and an explicit `detach()` on
     timeout (deliberately leaks the thread rather than blocking further) — applied to both
     concurrency tests that launch `Execute()` in the background.
  3. Documented-not-fixed: `pipeline.status_lock()` is held across the holder-pull branch's
     unbounded `Pull()`. Harmless today (Stage 4 is strictly one active drain per pipeline,
     so nothing else contends for that lock while it's held) but flagged with a code comment
     for whoever builds intra-pipeline task parallelism next, since that phase will need
     this same lock available for a concurrent completion check.
  4. Minor hygiene: `scheduled_`/`full_dependents_` now cleared at the end of `Execute()`
     too (previously only at the top of the next call); `MarkFinished()` now fires
     unconditionally via the same RAII guard as the holder `Finish()` call, so a failed
     pipeline isn't left in a permanently ambiguous state.
- Regression tests added beyond the original 6: `FullBarrierCycleAmongSubsetOfPipelinesFailsInsteadOfHanging`.

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
