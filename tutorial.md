# Tutorial: Workflow Orchestration for Claude Code

A practical guide to the CLAUDE.md-driven workflow used by Boris Cherny (creator of Claude Code), who runs 10-15 parallel Claude sessions daily. The core idea: Claude writes rules for itself after every mistake, so it gets smarter on your codebase over time.

---

## Prerequisites

- Claude Code installed and working
- A project you're actively building
- Familiarity with `CLAUDE.md`, subagents, and plan mode

---

## Step 1: Set Up the File Structure

Before writing any code, create two files in your project root:

```
your-project/
├── CLAUDE.md          # Rules and workflow instructions
├── tasks/
│   ├── todo.md        # Current task plan with checkable items
│   └── lessons.md     # Accumulated mistakes and prevention rules
```

Create them:

```bash
mkdir -p tasks
touch tasks/todo.md tasks/lessons.md
```

`tasks/todo.md` is your active task tracker. `tasks/lessons.md` is the institutional memory — every mistake Claude makes gets logged here with a prevention rule, so the same error never happens twice.

---

## Step 2: Add the Workflow Orchestration Rules to CLAUDE.md

Paste the following into your project's `CLAUDE.md`. These rules tell Claude *how to work*, not what to build.

```markdown
## Workflow Orchestration

### 1. Plan Mode Default
- Enter plan mode for ANY non-trivial task (3+ steps or architectural decisions)
- If something goes sideways, STOP and re-plan immediately — don't keep pushing
- Use plan mode for verification steps, not just building
- Write detailed specs upfront to reduce ambiguity

### 2. Subagent Strategy
- Use subagents liberally to keep main context window clean
- Offload research, exploration, and parallel analysis to subagents
- For complex problems, throw more compute at it via subagents
- One task per subagent for focused execution

### 3. Self-Improvement Loop
- After ANY correction from the user: update `tasks/lessons.md` with the pattern
- Write rules for yourself that prevent the same mistake
- Ruthlessly iterate on these lessons until mistake rate drops
- Review lessons at session start for relevant project

### 4. Verification Before Done
- Never mark a task complete without proving it works
- Diff behavior between main and your changes when relevant
- Ask yourself: "Would a staff engineer approve this?"
- Run tests, check logs, demonstrate correctness

### 5. Demand Elegance (Balanced)
- For non-trivial changes: pause and ask "is there a more elegant way?"
- If a fix feels hacky: "Knowing everything I know now, implement the elegant solution"
- Skip this for simple, obvious fixes — don't over-engineer
- Challenge your own work before presenting it

### 6. Autonomous Bug Fixing
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
```

This is the base. You'll extend it with project-specific rules over time.

---

## Step 3: Understand Each Rule and Why It Exists

### 3.1 — Plan Mode Default

**The problem it solves:** Claude jumps into implementation too fast, builds the wrong thing, then you spend 10 turns debugging a decision that should have been caught upfront.

**How it works in practice:**

When you give Claude a non-trivial task, it should enter plan mode first. A non-trivial task is anything with 3+ steps or any architectural decision.

Example — what NOT to do:
```
You: "Add user authentication to the API"
Claude: *immediately starts writing auth middleware*
```

Example — what the rule enforces:
```
You: "Add user authentication to the API"
Claude: *enters plan mode*
  - What auth strategy? (JWT, session, OAuth)
  - Where does the middleware go?
  - What endpoints need protection?
  - What's the session storage?
  - Writes plan to tasks/todo.md
  - Checks in with you before writing code
```

**The critical sub-rule:** If something goes sideways mid-implementation, Claude should STOP and re-plan. The default behavior is to keep pushing forward, patching problems on top of problems. That creates context rot and hacky code. Stopping and re-planning is almost always faster.

### 3.2 — Subagent Strategy

**The problem it solves:** Your main context window fills up with research, exploration, and dead-end debugging, degrading Claude's performance on the actual task.

**How it works in practice:**

Think of your main Claude session as a project manager. It should delegate, not do everything itself.

| Task type | Where it runs | Why |
|---|---|---|
| Research (docs, APIs, patterns) | Subagent | Keeps research noise out of main context |
| Exploration (prototyping approaches) | Subagent | Failed approaches don't pollute main context |
| Parallel analysis (review 3 files) | 3 subagents | Each gets focused context on one file |
| Implementation | Main session | Needs full project context |
| Bug fixing (isolated) | Subagent | Debugging history stays contained |

The rule "one task per subagent" is critical. A subagent with a narrow, well-defined task performs dramatically better than one juggling multiple concerns. If you have 3 things to investigate, launch 3 subagents, not 1.

To use this, just say: "Use a subagent to research X" or "Launch subagents to analyze files A, B, and C in parallel."

### 3.3 — Self-Improvement Loop

**The problem it solves:** Claude makes the same category of mistake repeatedly across sessions because it has no memory of previous corrections.

**This is the most important rule in the entire file.** Boris Cherny's team uses this exact pattern: after every correction, Claude updates its own rules so the mistake never happens again. The longer you use it, the smarter it gets on your codebase.

**How it works in practice:**

1. Claude does something wrong (wrong pattern, bad assumption, ignored convention)
2. You correct it
3. Claude fixes the code AND writes a lesson to `tasks/lessons.md`
4. At the start of each session, Claude reviews `tasks/lessons.md`

Example `tasks/lessons.md` entry:

```markdown
## Lesson #4: Don't use raw SQL for BigQuery queries
**Date:** 2026-04-01
**What happened:** Used string concatenation for a BigQuery query parameter, creating injection risk.
**Rule:** Always use parameterized queries via the BigQuery client library. Never concatenate user input into SQL strings.
**Prevention:** Before writing any database query, check if the client library has a parameterized query method. Use it.
```

The key instruction to Claude after any correction:

> "Update tasks/lessons.md so you don't make that mistake again."

This single sentence is what makes the loop work. Claude writes the rule. You review it. Next session, Claude reads it and avoids the mistake.

### 3.4 — Verification Before Done

**The problem it solves:** Claude declares "done" without actually proving the code works. You discover the bug later, after context has moved on.

**How it works in practice:**

Before Claude says "task complete," it must:

1. **Run the tests** — not just write them, actually execute them and show passing output
2. **Check logs** — if it's a server change, show the server running without errors
3. **Diff against main** — show what changed and why, so you can spot unintended side effects
4. **Apply the staff engineer test** — "Would a staff engineer approve this PR?" If the answer is "they'd ask questions," Claude should ask those questions itself first

This rule turns Claude from a code generator into a code *shipper*. The difference matters.

### 3.5 — Demand Elegance (Balanced)

**The problem it solves:** Claude's default is to produce the first working solution, which is often hacky. But the opposite extreme — over-engineering simple fixes — wastes time.

**How it works in practice:**

For non-trivial changes, Claude should pause after writing a solution and ask itself: "Is there a more elegant way?" If the answer is yes, it should implement the elegant version.

The trigger phrase for when you see a hacky fix:

> "Knowing everything you know now, scrap this and implement the elegant solution."

This works because Claude now has the full context of the problem (including the failed approach), so it can make a better architectural decision on the second pass.

**When to skip:** Simple bug fixes, config changes, one-line patches. Don't over-engineer the obvious.

### 3.6 — Autonomous Bug Fixing

**The problem it solves:** Claude asks too many clarifying questions when the answer is already in the logs, errors, or failing tests.

**How it works in practice:**

When you paste a bug report or error, Claude should:

1. Read the error message
2. Find the relevant code
3. Identify the root cause
4. Fix it
5. Run the tests to confirm

No questions like "What file is this in?" or "Can you show me the error?" — Claude has access to the codebase and can find that itself.

This rule is about **zero context switching for the user**. You paste the bug, walk away, come back to a fix. If Claude can't figure it out autonomously, *then* it asks.

---

## Step 4: The Task Management Workflow

This is the operational loop you'll use for every task. It's not complicated, but the discipline matters.

### Starting a new task

```
You: "I need to add rate limiting to the API endpoints"
```

Claude should:

1. **Enter plan mode** (Rule 1)
2. **Write the plan to `tasks/todo.md`:**

```markdown
# Rate Limiting for API Endpoints

## Plan
- [ ] Research rate limiting strategies (token bucket vs sliding window)
- [ ] Choose strategy and define limits per endpoint
- [ ] Implement rate limiter middleware
- [ ] Add Redis/in-memory store for rate tracking
- [ ] Add rate limit headers to responses (X-RateLimit-*)
- [ ] Write tests for rate limiting behavior
- [ ] Test edge cases (burst traffic, limit reset)
- [ ] Update API docs

## Status: Planning
```

3. **Check in with you** before implementing (Step 2 of task management)
4. **Mark items complete** as it goes
5. **Add a review section** at the end:

```markdown
## Review
- Rate limiter uses token bucket algorithm with Redis backend
- Limits: 100 req/min for authenticated, 20 req/min for anonymous
- All tests passing (12 new tests added)
- Headers added: X-RateLimit-Limit, X-RateLimit-Remaining, X-RateLimit-Reset
```

6. **Capture lessons** if any corrections were needed during the process

### Between sessions

At the start of each new session, Claude should review:
- `tasks/todo.md` — what's in progress
- `tasks/lessons.md` — what mistakes to avoid

This is what gives Claude persistent project intelligence across sessions.

---

## Step 5: The Parallel Session Workflow

Boris runs 10-15 sessions simultaneously: 5 in terminal, 5-10 on web. Here's how to think about that.

### When to use parallel sessions

| Scenario | Sessions | Approach |
|---|---|---|
| One focused feature | 1 terminal | Standard workflow |
| Feature + bug fix + docs | 3 terminal | Each session has isolated context |
| Exploring 3 approaches | 3 terminal | Race them, pick the winner |
| Feature + research + review | 2 terminal + 1 web | Terminal builds, web researches/reviews |
| Large refactor across modules | 5+ terminal (git worktrees) | Each agent works on isolated branch |

### How to manage them

- **`/rename`** each session: `[auth-middleware]`, `[rate-limiter]`, `[docs-update]`
- **`/resume`** when you need to come back to one
- **Git worktrees** for parallel development on the same repo without conflicts:
  ```bash
  git worktree add ../project-auth feature/auth
  git worktree add ../project-rate-limit feature/rate-limit
  ```
- Each worktree gets its own Claude session with isolated context

### The coordination problem

When multiple agents work on the same codebase, you need to manage merges carefully. Each agent should work on isolated files/modules when possible. If two agents need to touch the same file, make them sequential, not parallel.

---

## Step 6: Making It Stick

### Week 1: Foundation
- Add the CLAUDE.md rules to your project
- Create `tasks/todo.md` and `tasks/lessons.md`
- Use plan mode for every non-trivial task
- After every correction, say: "Update tasks/lessons.md so you don't make that mistake again"

### Week 2: Context discipline
- Start using subagents for research and exploration
- Monitor context usage with a status line
- Practice `/compact` and `/clear` at the right times
- Use double-escape to trim debugging context after bug fixes

### Week 3: Parallel work
- Try running 2-3 sessions simultaneously
- Use `/rename` and `/resume` to track them
- Experiment with git worktrees for isolated branches

### Week 4: Review and iterate
- Read through `tasks/lessons.md` — look for patterns
- Tighten your CLAUDE.md rules based on what you've learned
- Remove rules that don't help, strengthen ones that do
- Your CLAUDE.md should be getting more specific to your project over time

---

## Common Mistakes

| Mistake | Fix |
|---|---|
| CLAUDE.md grows too long (200+ lines) | Split into `.claude/rules/` files or use `<important if="...">` tags for conditional rules |
| Claude ignores rules in CLAUDE.md | Wrap critical rules in `<important>` tags. Keep file under 200 lines. Use `@path` imports for large rule sets |
| Lessons file becomes a dump | Review and prune weekly. Merge similar lessons. Remove ones that are no longer relevant |
| Too many subagents, losing track | Give each a narrow, specific task. Don't launch more than you can review |
| Context rot in long sessions | `/compact` at 50% context. `/clear` at clean breakpoints. Don't let sessions run past 70% context |
| Claude keeps asking permission | Use `/sandbox` for file/network isolation (84% reduction). Or `/permissions` with wildcard syntax for surgical grants |

---

## The One-Sentence Summary

Drop these rules into your CLAUDE.md, enforce the lesson-logging loop after every correction, and Claude gets progressively smarter on your specific codebase — turning a generic AI into a project-specific engineering team.