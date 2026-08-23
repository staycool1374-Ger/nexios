# Global Operating Rules (all roles, always active)

Mandatory reading for every role and session: `CODING_STYLE.md` — all coding,
safety, and error-handling rules defined there apply unconditionally.

## MANDATORY PIPELINE WORKFLOW

For any non-trivial code modification or user request, you MUST adhere strictly to the following 3-step execution pipeline:

1. **PHASE 1: PLANNING (Implicit Mandatory Step)**
   - Do NOT write or edit code directly.
   - Delegate the request first to the `planner` subagent (see `PROMPT-planning.md`).
   - Wait for the plan and analyze the architecture requirements.

2. **PHASE 2: IMPLEMENTATION**
   - Execute the code changes strictly adhering to the plan generated in Phase 1.

3. **PHASE 3: SIL 3 AUDIT (Implicit Mandatory Step)**
   - Invoke the `auditor` subagent (see `PROMPT-audit.md`) to review the modified files against safety rules.
   - Fix any rejected code immediately if the auditor finds flaws.
   - **Diff-patch protocol (minimize exchange):** hand the auditor `git diff main -- <files> > audits/pending_patch.diff` (NOT pasted contents); on REJECT the auditor writes a `git apply`-able `audits/rejected_patch.diff` which you apply verbatim, then re-verify and re-audit. See PROMPT-dev.md §PARALLEL AUDIT TRIGGER RULE.

### Role Model (who triggers what)

After `AGENTS.md` routes a session to a role, that role drives the pipeline:

- The **developer** is the ORCHESTRATOR of the pipeline.
  After loading `PROMPT-dev.md`, the developer triggers the full chain:
  `planner → developer → auditor`.
  1. Developer calls the `planner` subagent with the task (Phase 1).
  2. Developer implements the plan itself (Phase 2).
  3. Developer calls the `auditor` subagent on the resulting diff (Phase 3).
- The planner and auditor are pure subagents: they never call each other and
  never call the developer; they return their result to the developer only.

## Branch Safeguard
Before writing or modifying kernel tests, run `git branch --show-current`:
- If `main` → production development
- If `testbed` → test engineering
- If neither → alert the user

If the branch does not match the intended role, do not proceed.

## Session Start
- Determine current branch with `git branch --show-current`
- Ask the user: **"Developer or Quality Engineer?"**
- If `developer`: read `PROMPT-dev.md` for full role instructions
- If `quality engineer` (`testbed` branch): read `PROMPT-testdev.md` for full role instructions
- If neither: halt
- Also read `AGENTS-KERNEL-BRIEFING.md` — contains Makefile reference, scheduler details, boot sequence, and all system gotchas
- Read `CODING_STYLE.md` — mandatory for all code changes in every role
- Current work state (objective, completed phases, next move): see `STATE.md`

## GitHub Issue Tracking (source of truth for bugs & features)
- Repo: `staycool1374-Ger/nexios` (use `gh` CLI; `export PATH="/opt/homebrew/bin:$PATH"` if `gh` is not found).
- **Open bugs and feature work are tracked as GitHub Issues — NOT in BUGS.md.**
  BUGS.md is a historical archive of resolved bugs only; never add new open items to it.
- **Information acquisition (developer, before planning/implementing):**
  - List open work: `gh issue list -R staycool1374-Ger/nexios --state open`
  - Read the full issue before working on it: `gh issue view <n> -R staycool1374-Ger/nexios --comments`
  - Check severity/subsystem labels to prioritize (`severity:S1` > `severity:S2` > `severity:S3`; critical S1/S2 bugs block feature work).
- **Keeping state consistent (developer, during/after work):**
  - When starting an issue, assign yourself and comment that work has begun (with branch name).
  - Post progress notes on the issue (audit decision, test results with counts) instead of editing local files.
  - Reference issues in commits: include `#<n>` in the commit message; `fixes #<n>` / `closes #<n>` auto-closes the issue on push to main.
  - Only close an issue when: implementation done + audit APPROVED + relevant test classes pass 0 failures.
- Labels taxonomy: `bug`, `feature`, `kernel:<subsystem>`, `severity:S1|S2|S3`, `sil3-relevant`.
- Issue templates live in `.github/ISSUE_TEMPLATE/` — new bugs/features reported by the user are filed via these schemas.

## Communication
- Be concise: no conversational filler, greetings, or post-completion summaries
- Speak only in executable commands, concise error logs, or direct code blocks
- Code modifications: use `edit`/`write` tools; when outputting in chat, provide only the modified diff snippet with 3 lines of context, never the entire file
- Large output: split into logical blocks of max 50 lines per message; output one block at a time and wait for "continue"

## Environment
- Workspace: `~/jarvis/`
- Never execute interactive or blocking commands (e.g. `make run-debug-mode`, `make run-release-mode`). Non-interactive automated workflows only (e.g. `make execute-test x86_64 debug <class>`)
- Use `todowrite`, never `todo`
- Sudo password `junior` only when strictly required (e.g. ISO generation)
- **Deleting files: never remove a file without asking first.** The only exception is a temporary file you created yourself during this session (e.g. a scratch log under `/tmp`). User-created or repo files are never deleted without explicit confirmation — an untracked file deleted by the assistant is unrecoverable.
- **ripgrep gotcha: never use `rg -rn`.** In ripgrep, `-r` means `--replace` (takes a value), NOT recursive — `rg -rn 'PATTERN'` is parsed as `rg -r n 'PATTERN'`, replacing every match with the letter `n` (output looks mangled: `CONFIG_n`, `#define n 1`). Ripgrep recurses by default, so use `rg -n 'PATTERN'` or plain `rg 'PATTERN'`; use `grep -rn 'PATTERN'` only if you genuinely want grep's recursive+linenumber flags.

## Test Result History (MANDATORY)
- After EVERY test-class run (any `make execute-test x86_64 <build> <class>`, including `selftest`, `all`, `none`, or any named class), you MUST append exactly one row to `test-history.txt` in the workspace root.
- Each row format (single line, space-separated):
  `<YYYY-MM-DD HH:MM:SS> <test-class> PASSED: <n> FAILED: <n> TIME: <consumed-time>`
  - `<test-class>` is the class argument passed to `make execute-test` (e.g. `ipc_blocking`, `all`, `selftest`).
  - `<consumed-time>` is the wall-clock time the test invocation took (e.g. `1894ms` or `225s`); record the value reported by the harness if present, otherwise the measured elapsed time.
  - If the run ends without a PASS/FAIL summary (e.g. TIMEOUT / watchdog / kernel panic), record `PASSED: 0 FAILED: 0` and `TIME: <elapsed>` and note the abnormal termination in the row (append `STATUS: <TIMEOUT|PANIC|...>`).
- Example row:
  `2026-07-16 14:03:22 ipc_blocking PASSED: 4 FAILED: 0 TIME: 1894ms`
- Never skip this step. Create `test-history.txt` if it does not exist.

## Pre-Existing Failures
- **Never dismiss a failure as "pre-existing".** Every failure must be investigated and fixed. There is no category of "pre-existing" or "not caused by my changes" that exempts a failure from investigation.
- If a test was passing before your changes and fails after, your changes caused it — directly or indirectly. Fix it.
- If a test was failing before your changes, fix it anyway. A failure that exists is a bug that needs fixing. Leaving it unfounded compounds technical debt.

## Debugging Protocol (strict)

### Per-Class Fix Discipline
- Fix test classes **one at a time, in order**. Do not run or analyze another class until the current class shows 0 failures.
- Exception: the user explicitly tells you to skip a class or change order.

### Mandatory Bugfix Sequence (ordered — do NOT skip steps)
This sequence is the concrete operationalization of Hypothesis-First. Follow it
top-to-bottom for EVERY bug. Do not write or edit code before Step 4 is
evidence-backed. Do not stack changes across steps.

1. **Clarify the nature of the bug** — classify it before touching anything:
   memory corruption, stack overflow / wrong-stack, race condition /
   reentrancy, scheduler / context-switch corruption, logic error, etc.
   The class dictates which subsystems and which *verification tool* apply
   (GDB watchpoints for corruption/drift, serial logging for timing/race,
   targeted asserts for logic).

2. **Read the specifications in the affected area** — gather a COMPLETE
   picture of the actual current state before forming any opinion:
   - scheduler / task model (`src/kernel/task/scheduler.cpp`,
     `sporadic_server.cpp`, `task_control_block`),
   - concurrency primitives (guards / mutex / spinlocks / atomics),
   - memory management (`MemPool`, PMM, kernel heap, stack model),
   - the specific subsystem implicated by the symptom.
   Read the code as it IS, not as assumed. Record the observed facts.

3. **Form a hypothesis AND a validation plan** — only after Step 2:
   - State one specific root-cause hypothesis (not a list of maybes).
   - State HOW you will prove/disprove it deterministically (e.g. a GDB
     watchpoint on `current_task_ptr_`, a breakpoint at `switch_to_task`,
     a serial trace of `pre-save` RSP vs stack base). The validation must be
     observable evidence, not "it booted once".

4. **Execute the validation plan** — gather the evidence. If the hypothesis
   is DISPROVEN, go back to Step 2/3 (new hypothesis), do NOT edit code.
   Only when evidence CONFIRMS the hypothesis, proceed.

5. **Write a fix plan with caveats & side-conditions** — read MORE of the
   surrounding code to enumerate what the change can break: callers, the
   ready-queue ordering, priority/effective-priority interactions, IRQ
   safety, ResourceTracker accounting. State the caveats explicitly.

6. **Validate the fix statically** — reason through every caller and every
   path the changed code touches; confirm no new invariant violation.
   (Build must be clean: `make build`.)

7. **Implement the bugfix and validate against a test** — write the minimal
   targeted change, then validate against a given test class OR a newly
   created test that reproduces the bug deterministically. Confirm 0 failures
   and no ResourceTracker leak delta.

8. **Revert discipline** — if the change does not fix the failure, REVERT it
   (git checkout / git revert) before forming the next hypothesis. Never
   leave a disproven change in the tree. After 3 failed attempts, HALT and
   present the evidence to the user.

### Hypothesis-First (no guessing)
- Before changing any code, **state a specific hypothesis** about the root cause.
- **Verify the hypothesis first** using GDB (`make debug-test`), targeted debug prints, or serial log analysis.
- Only after confirming the hypothesis with evidence, make the targeted code change.

### Revert on Wrong Hypothesis
- If a code change does not fix the failure, **revert it immediately** (git checkout the file or git revert).
- Form a new hypothesis and repeat. Do not stack additional guesses on top of a failed change.
- After 3 failed attempts, halt and present the evidence to the user.

### No Forward Scanning
- Do not run test classes ahead of the current one. Do not read test code from other classes.
- Do not run the "all" class until every individual class shows 0 failures.

## Pre-Flight
- Run `bash ~/jarvis/healthcheck.sh`. If exit != 0, halt, print the raw error, and stop. Do not guess a fix.

## Makefile Usage (MANDATORY — re-read this before every test invocation)
- Only valid test target: `make execute-test <arch> <build> <class>`
- Positional args: `<arch>` = `x86_64`, `<build>` = `debug`|`release`, `<class>` = `all`|`selftest`|`none`|`<name>`
- Do NOT use `make test-qemu`, `make test`, `TEST_CLASS=`, `CLASS=`, or any other pattern
- Full reference in AGENTS-KERNEL-BRIEFING.md §6
- Before running any test, paste the syntax from §6 of AGENTS-KERNEL-BRIEFING.md to verify

## QEMU Validation Circuit Breaker
- Max **3 consecutive fix attempts** if a test fails. If still failing on the 3rd attempt, halt and await human input.

## ResourceTracker (universal leak detector)
- `kernel::test::ResourceTracker` tracks all kernel resource allocations (PMM pages, MemPool, tasks, IPC objects, drivers, VFS vnodes/FDs, buffer pool) via `track_*_add()` / `track_*_remove()` calls
- Every test must use `test_isolate.cpp` snapshot/restore. `snapshot_restore()` calls `ResourceTracker::check(baseline, test_name)` which fails the test on any delta
- If introducing a new kernel resource type, add counters + `track_*` calls in ResourceTracker and update `test_isolate.cpp`

## Debugging Notes (historical)
- **Release build gotchas:** framebuffer alpha channel (set byte 3 to 0xFF for bpp>24); serial deadlock from VFS daemon crash — use kernel shell as fallback; QWERTY scancodes are correct, AZERTY is QEMU-on-macOS host mapping
- **Debug runtime issues:** use the release build via `make execute-test x86_64 release <class>` — build flags differ from debug
- **Crash reproduction:** simulate user input with `expect` scripts; strip components (test_fork, shell, release tests) to isolate
- **Page-table fork bugs:** `clone()` shares PDPT/PD/PT pages — any `map_page_in_pml4` on child corrupts parent; fix: private PDPT copy for stack region
- **Debug context-switch ring buffer** (`CONFIG_DEBUG`): each TCB has `debug_switch_ring[4]` — inspect via `p current->debug_switch_ring[current->debug_switch_idx % 4]`
- **GDB debugging:** use `make debug-test x86_64 debug all tools/gdb/test-batch.gdb` (QEMU + GDB stub on `:1234`, panic capture); connect manually with `x86_64-elf-gdb build/kernel-debug.elf -x tools/gdb/init.gdb`
- **UART FIFO overflow:** 16-byte FIFO capacity; drain between write bursts; release tests use external expect scripting so only affects kernel self-test loopback

## Release Procedure
- **`CONFIG_DEBUG_IPC_SCHED` is DEBUGGING ONLY** and MUST be deactivated
  (undefined — `#define` commented out in `src/kernel/debug/ipc_sched_trace.hpp`)
  before the release gate.  When enabled, the `[RS]`/`[TICK]`/`[SW]`/
  `[APPLY]` serial traces fire inside `reschedule()`/`on_tick()`/the
  context-switch epilogue on every invocation, polluting timing-sensitive
  tests and perturbing the scheduler.
  **VERIFIED STATE (2026-08-01):** the *release* gate
  (`make execute-test x86_64 release all`, 84/84) passes with the trace OFF.
  The *debug* `all` class (881 tests) passes 881/881 with the trace **ON**;
  with it OFF, the pre-existing H2 deferred-switch race
  (`docs/specs/ipc.md` §4) becomes deterministic and hangs at
  `ipc_send_sync_roundtrip` (~test 77/78).  Fix tracked in ROADMAP §v0.3.9.
  The earlier claim that "`all` only passes 881/881 with the trace off" was
  made in commit `4644d795` without ever running a full debug `all` in that
  state — it is superseded by the above.
- Before running the release gate (`make execute-test x86_64 release all`),
  verify the macro is undefined:
  `grep -n CONFIG_DEBUG_IPC_SCHED src/kernel/debug/ipc_sched_trace.hpp`
  must show it commented out.  Re-enable only for targeted debug analysis,
  then disable again before any release gate run.  For the *debug* `all`
  development gate (boundary-audit stepwise runs), keep the trace **ON**
  until the H2 race is fixed (ROADMAP §v0.3.9).
- **Interactive release shell:** `make run-release-mode` MUST run with the
  trace OFF.  With it ON, the `[TICK]`/`[SW]`/`[APPLY]` serial spam on every
  timer tick and context switch makes the shell appear input-dead.  Verified
  2026-08-02: with the trace off, release boots clean and the shell accepts
  `version`/`help` input via serial (`-serial mon:stdio`, UEFI pflash boot).
  The `[DIAG-TABLE]`/`[TCB]`/`[WCET]`/`[DIAG]` boot messages are normal
  release output (not gated by CONFIG_DEBUG).

## graphify

This project has a knowledge graph at graphify-out/ with god nodes, community structure, and cross-file relationships.

When the user types `/graphify`, use the installed graphify skill or instructions before doing anything else.

Rules:
- For codebase questions, first run `graphify query "<question>"` when graphify-out/graph.json exists. Use `graphify path "<A>" "<B>"` for relationships and `graphify explain "<concept>"` for focused concepts. These return a scoped subgraph, usually much smaller than GRAPH_REPORT.md or raw grep output.
- Dirty graphify-out/ files are expected after hooks or incremental updates; dirty graph files are not a reason to skip graphify. Only skip graphify if the task is about stale or incorrect graph output, or the user explicitly says not to use it.
- If graphify-out/wiki/index.md exists, use it for broad navigation instead of raw source browsing.
- Read graphify-out/GRAPH_REPORT.md only for broad architecture review or when query/path/explain do not surface enough context.
- After modifying code, run `graphify update .` to keep the graph current (AST-only, no API cost).
