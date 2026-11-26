# PLAN-516: Kill-site death handshake at the kill decision (single choke point) — REVISION R2

**Issue:** #516 (S2, sil3-relevant, `kernel:process`) — repo `staycool1374-Ger/nexios`, branch `main`, dev lane.
**Revision basis:** closes all findings of `audits/report-20261126-516-plan.md` (1×S1, 3×S2, 3×S3). Section markers `[F1]`–`[F7]` map finding → change.
**Evidence base:** issue body (fault-kill via `deliver_signal_to_user` → `terminate_err` never reaches `daemon::notify_death`, supervisor PID cell keeps the dead PID, `restart_stale_daemons` skips it), tree inspection at HEAD, `docs/specs/death-notify.md`, `prompts/LEARNINGS.md` (#105 Part B, #523, #559, #542, cautions 20/22/42/44/55/62/78/79).
**Retrieval artifacts:** `graphify query_graph "death notify publish_task_death supervisor restart deferred kill process teardown"` — used: graph places `notify_death()` (daemon_mgr.cpp:123) in community 338 with `restart_stale_daemons()` and `publish_task_death`/`on_task_death` in community 271, i.e. two distinct functions (corroborates [F2]); `obsidian_search_vault` ("death-notify", "DeathCause", "#516") — used: `specs/death-notify.md:68` (funnel wording), `audits/plan-593.md` (admin-funnel precedent), `audits/design-review-557-*` (watch-vs-stop authority); ignored: `specs/microkernel-transition*.md` (unrelated #516 text hits).

## AFFECTED FILES

- `src/kernel/task/scheduler_terminate.cpp` — `publish_task_death` (L47-88) stays the single *record-latch* choke point; new `note_death_cause` helper replaces the inline `if (dead.death_cause == 0)` at L60-62.
- `src/kernel/task/scheduler_lifecycle.cpp` — `defer_kill` (L1103-1123) gains a `DeathCause`; parallel cause array defined TU-local beside `s_deferred_kill_gens` (L53-56); `process_deferred_kills` (L1125-1187) reordered (AP-live first) and publishes at the decision; new `reset_deferred_kills_for_test()` hook.
- `src/kernel/task/scheduler.hpp` — `defer_kill` signature + doc (L1355), `publish_task_death` doc-block (L1246), stale `defer_kill()` prose (L1901, L1919), `reset_deferred_kills_for_test()` decl beside the other `*_for_test` seams (L373 area).
- `src/kernel/task/scheduler_watchdog.cpp` — the two KILL arms (L317-331, L475-489) `note_death_cause` + `defer_kill(&task, cause)` **before** `wake_waiting_parent`, and the `PARENT_COLLECTS` `return` deleted.
- `src/services/shell.cpp` — `background_task_wrapper` (L257-263) runs the real self-exit funnel; **one new include** `kernel/ipc/death_notify.hpp`.
- `docs/specs/death-notify.md` — new "Kill-site coverage matrix" §, restated invariants, PARK/canary/factory-reset non-goals.
- `src/kernel/ipc/death_notify.hpp` — comment-only: enumerate driving call sites (L103-105); `Watchdog` enum comment gains its producer.
- Test tree (`src/kernel/test/**`, `test_expected_counts.hpp`, `test_registry.cpp`) — **QE-owned on `testbed`**, not touched by this lane; carries the new test file and the `test_isolate.cpp` isolation-reset call (see TEST STRATEGY).
- *Not changed:* `scheduler_lifecycle_internal.hpp` — the cause array mirrors `s_deferred_kill_gens` (TU-local `static`), so no cross-TU extern is added; isolation uses the reset hook instead ([F4]).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-K1 (single latch choke point — scoped) [F2].** `Scheduler::publish_task_death` is the only producer of a death *record*: the sole production caller of `ipc::DeathNotify::on_task_death` is `scheduler_terminate.cpp:68`, and the only production callers of it are that funnel plus the teardown backstop, which itself calls the funnel (`task.cpp:3089`). `kernel::daemon::notify_death` (`daemon_mgr.cpp:123`) is a **different function** — the daemon-manager PID-cell clear — and its owner-legitimate callers are: `publish_task_death` (`scheduler_terminate.cpp:63`), `complete_teardown` (`task.cpp:3082`), and the daemon manager clearing its own registry (`daemon_mgr.cpp:192`, `:480`). Those four are the enumerated allowlist; INV-K1 does not forbid them. Enforcement: rewritten grep gate (Step 9).
- **INV-K2 (context discipline, unchanged).** `publish_task_death` ENSUREs `scheduler_lock_depth_own() == 0` (`scheduler_terminate.cpp:55`), takes its own `arch::IrqGuard`, takes only leaf locks (`DeathNotify::s_lock_`, proc-cap table), bounded loops, **no allocation, no blocking, no `reschedule()`/`switch_to_task`**. New call sites satisfy the same contract.
- **INV-K3 (cause first-writer-wins) [F3].** `death_cause` is written exactly once per life: CAS-on-zero only. A recycled TCB must start at 0 — verify **five** memset-zero birth sites (`task.cpp:1072` `create_carved`, `:1311` `create`, `:1599` `create_user`, `:2027` `clone`, `:2458` ELF finalize) **plus** the ctor init-list zero (`task.hpp:392`, `death_cause(0)`; LEARNINGS caution #20). A stale non-zero value silently suppresses the latch.
- **INV-K4 (exactly-once record).** `DeathNotify::on_task_death` transitions `ACTIVE→PENDING` only; a repeat publish is a no-op (`death_notify.cpp:128-162`). Deferred publish + teardown backstop cannot duplicate.
- **INV-K5 (lock order).** `scheduler_lock_ → DeathNotify::s_lock_ → leaf`. Publish is never called with `scheduler_lock_` held — which is why the watchdog fix queues intent in the arm and publishes in the `on_tick` lock-free gated tail (the idle-seam arm `fire_stall_watchdog` holds `scheduler_lock_` at `scheduler_watchdog.cpp:421`).
- **INV-K6 (no supervisor ⇒ no-op).** Zero slots matched ⇒ no poke, no wake, no error, no block. Registry full ⇒ registration fails closed. No new failure mode on the dying path.
- **INV-K7 (no stored supervisor pointer).** Slots keep `(supervisor_id, supervisor_gen)`; poke re-resolves via `find_task` + `task_live` + generation compare (`death_notify.cpp:166-171`). No retained `TaskControlBlock*` is added.
- **INV-K8 (queue + isolation coherence) [F4].** `(tasks[i], gens[i], causes[i])` are one record: every write, retain-copy and drop is applied to all three at the same index, **never a subset**. `s_deferred_kill_count` is the **commit word** — written last on append, and set to `kept` last after compaction, so no consumer ever reads a cause slot without its task/gen. Full-list leg writes nothing (all three drop in lockstep). `reset_deferred_kills_for_test()` zeroes count + all three arrays, and is the designated snapshot/quiesce reset point (INV-K8a).
- **INV-K9 (no AP-live death lie) [F1].** Publish only for a victim that is (a) `state == TaskState::TERMINATED`, (b) `!teardown_completed_`, (c) `!teardown_claimed_` and still resolvable by id, and (d) **`!is_current_on_any_cpu(task)`**. Ordering is mandatory: the AP-live test precedes the publish decision; an AP-live victim is retained with its cause carried and announces on a later pass after it deschedules. No record is ever fabricated for a still-executing corpse.

## STEP-BY-STEP CHANGES

**Step 0 — Kill-site inventory (verified, do not re-derive).** Funnel column = path to `publish_task_death`.

| # | Site | Anchor | Funnel | Supervisor handshake today |
|---|---|---|---|---|
| 1 | x86 fatal SIGKILL precedence | `src/kernel/kernel.cpp:2353-2362` | `terminate_err` | ✅ synchronous |
| 2 | x86 signal-frame write failure | `kernel.cpp:2392-2399` | `terminate_err` | ✅ |
| 3 | x86 default-action kill (#PF/GP/UD → SIGSEGV/…) | `kernel.cpp:2453-2459` | `terminate_err` | ✅ |
| 4 | aa64 signal stub kill | `kernel.cpp:2471-2475` | `terminate_err` | ✅ |
| 5 | rv64 signal stub kill | `kernel.cpp:2485-2489` | `terminate_err` | ✅ |
| 6 | aa64 unattributable-trap kill | `src/kernel/arch/aarch64/interrupt_controller.cpp:396-402` | `terminate_err_from_trap` | ✅ |
| 7 | aa64 EL0 fault kill | `arch/aarch64/interrupt_controller.cpp:504-509` | `terminate_err_from_trap` | ✅ |
| 8 | rv64 U-mode fault kill | `src/kernel/arch/riscv64/interrupt_controller.cpp:292-296` | `terminate_err_from_trap` | ✅ |
| 9 | rv64 unattributable-trap kill | `arch/riscv64/interrupt_controller.cpp:364-368` | `terminate_err_from_trap` | ✅ |
| 10 | `sys_kill` self-fatal | `src/kernel/proc/proc_svc.cpp:634-642` | `terminate_err_from_trap` | ✅ |
| 11 | OOM kill (PMM handler) | `kernel.cpp:1562-1568` | `terminate_err` | ✅ |
| 12 | daemon admin terminate | `src/kernel/daemon/daemon_mgr.cpp:462-477` | `terminate_err` (Admin) | ✅ |
| 13 | `sys_exit` | `src/kernel/syscall/syscall_handlers_misc.cpp:93-127` (publish `:110`) | direct publish | ✅ |
| 14 | thread exit | `src/kernel/syscall/syscall_handlers_thread.cpp:160-177` (`:170`) | direct publish | ✅ |
| 15 | debug syscall kills | `src/kernel/syscall/syscall_handlers_debug.cpp:143,279,342,678` | `terminate_err` | ✅ |
| 16 | kernel-task self-exit / trampoline | `src/kernel/task/task.cpp:205-221` (`:210`) | `terminate` | ✅ |
| 17 | IRQ-thread exit | `src/kernel/irq_thread.cpp:147,232` | `terminate` | ✅ |
| 18 | **watchdog KILL arm** | `src/kernel/task/scheduler_watchdog.cpp:317-331` | `defer_kill`→`destroy`→`cleanup` | ❌ **BYPASS (cause lost; unbounded on `PARENT_COLLECTS`)** |
| 19 | **deadline KILL arm** | `scheduler_watchdog.cpp:475-489` | `defer_kill`→`destroy`→`cleanup` | ❌ **BYPASS (same)** |
| 20 | **shell background wrapper** | `src/services/shell.cpp:257-263` | none | ❌ **BYPASS** |
| 21 | teardown backstop (destroy/reaper) | `src/kernel/task/task.cpp:3082-3090` | backstop | ✅ (late, cause=`Teardown`) |
| 22 | factory reset bulk kill | `src/kernel/task/taskdefs.cpp:712-719` | **backstop** | ✅ late — bulk `TERMINATED` + `release_zombie` + `drain_zombie_list` (`:712-718`) → `cleanup` → `complete_teardown` → `publish(Teardown)` (`task.cpp:2888-2936`, `:3089`); death msgs suppressed (`taskdefs.cpp:723`), `daemon::reset_clear_daemons()` follows (`:727`); intentional — non-goal [F5] |
| 23 | test-task cleanup | `src/kernel/task/scheduler_lifecycle.cpp:1051-1055` | `terminate_err` | ✅ (test-only) |

Bypass mechanism for #18/#19, precisely: both arms set `TERMINATED` + `exit_code` and then
(a) `PARENT_COLLECTS` branch → `dequeue_ready` + **`return` before `defer_kill`** → no publish at all until an unbounded parent collect (then only the `Teardown` backstop fires); and
(b) non-collected branch → `defer_kill` → `process_deferred_kills` → `destroy` → `cleanup` → publish with `DeathCause::Teardown`. `DeathCause::Watchdog` (enum value 4) has **zero producers** in-tree (`grep DeathCause::Watchdog` → no hits), contradicting `death-notify.hpp:55` and the spec's cause enumeration.

**Step 1 — `scheduler_terminate.cpp`: extract the cause store.** Add `static void Scheduler::note_death_cause(TaskControlBlock &dead, ipc::DeathCause cause) noexcept` — `__atomic_compare_exchange_n(&dead.death_cause, 0, value)` (single store, first writer wins). Replace the inline `if (dead.death_cause == 0)` block at L60-62 with a call. No other logic moves; `daemon::notify_death` and `on_task_death` keep their current positions inside the funnel (INV-K1 scope is unchanged by this step).

**Step 2 — `scheduler_lifecycle.cpp`: cause array + isolation hook [F4].**
- Define `static constinit ipc::DeathCause s_deferred_kill_causes[MAX_DEFERRED_KILLS] = {};` immediately beside `s_deferred_kill_gens` (L53-56), with a comment naming it "third parallel slot of the deferred-kill record (tasks/gens/causes), same TU-local visibility as gens — no extern, no header change".
- Define `void Scheduler::reset_deferred_kills_for_test() noexcept` in this file: `ENSURE(scheduler_lock_depth_own() == 0);` then zero `s_deferred_kill_tasks[]`, `s_deferred_kill_gens[]`, `s_deferred_kill_causes[]` across `MAX_DEFERRED_KILLS`, then `s_deferred_kill_count = 0` last. Doc-block: "snapshot/quiesce reset point; drops intents, never publishes — the cycle boundary returns to the boot baseline and the zombie drain precedes it."

**Step 3 — `scheduler_lifecycle.cpp: defer_kill` (L1103-1123) carries the cause [F4].**
- Signature `defer_kill(TaskControlBlock *task, ipc::DeathCause cause) noexcept`; keep `ENSURE(validate_task_ptr(…))` and the unchanged `kill_declined` early-out.
- Append under the capacity check, in this exact order (commit-word discipline): `s_deferred_kill_gens[n] = task->generation;` → `s_deferred_kill_causes[n] = cause;` → `s_deferred_kill_tasks[n] = task;` → `s_deferred_kill_count = n + 1;` (no post-increment on the array write; count last).
- Full-list leg unchanged and writes **nothing** to any of the three arrays (`Logger::warn` + `log::dmesg_push_sev(SCHED, kDmesgBase_SCHED+1)` stay); annotate "all three slots drop in lockstep — the record is never half-appended".
- Retain compaction in `process_deferred_kills`: every `kept` copy writes all three (`tasks[kept]`, `gens[kept]`, `causes[kept]`); no leg copies a subset; `s_deferred_kill_count = kept;` remains last.

**Step 4 — `scheduler_lifecycle.cpp: process_deferred_kills` publish at the kill decision, AP-live first [F1].** Inside the loop, in this order, after the magic/generation guard (L1144-1146):
1. **AP-live retention first (no publish):** if `is_current_on_any_cpu(task)` → `Scheduler::note_death_cause(*task, causes[i]);` then copy `tasks/gens/causes[kept]` (all three) and `++kept; continue;`. The cause rides the retained record and announces on a later pass once the target deschedules (#197: never free a live stack; never announce a live corpse).
2. **Publish decision (non-AP-live only):** `if (task->state == TaskState::TERMINATED && !task->teardown_completed_ && !task->teardown_claimed_ && Scheduler::find_task(task->id) == task) { Scheduler::note_death_cause(*task, causes[i]); publish_task_death(*task, causes[i]); }`
3. Then the unchanged `kill_declined(task)` warn-and-drop leg (L1151-1158), the sporadic-server `ScopedRef`/`on_completion` block (L1170-1180), `Logger::info`, and `TaskControlBlock::destroy(task)` — byte-identical semantics.
- Full `PARENT_COLLECTS`/`wait_collect_pending_` corpses satisfy the step-2 gate (`TERMINATED`, teardown unclaimed/uncompleted) and now latch with their real cause at the decision; the teardown backstop then loses the first-writer race and records nothing new (INV-K4). This ordering is the **default** for the one remaining owner question Q2b; the hold-until-teardown alternative is a two-token move of block (2) below `kill_declined` plus a `!task->wait_collect_pending_` term, and changes no other step.
- Do **not** add a lock; the tail already runs `IrqGuard`-only with `ENSURE(scheduler_lock_depth_own()==0)` (L1133) — the same discipline publish requires (INV-K2/INV-K5).
- Comment block above the loop: "ordering is load-bearing — AP-live retention precedes publish so a still-executing corpse is never announced (INV-K9); the publish decision precedes the drop leg so a parent-retained TERMINATED corpse latches with its true cause instead of the late `Teardown` backstop."

**Step 5 — `scheduler_watchdog.cpp`: both KILL arms route the intent through the queue.** In each arm, immediately after `state = TERMINATED` + `exit_code` (`:317-319`, `:475-477`):
- `Scheduler::note_death_cause(task, DeathCause::Watchdog)` — watchdog arm; `DeathCause::Kill` — deadline arm (cause split is Q1).
- `Scheduler::defer_kill(&task, <same cause>)` **before** `wake_waiting_parent`, and delete the `PARENT_COLLECTS` `Scheduler::dequeue_ready(task); return;` + trailing `defer_kill(&task)` pair so neither branch escapes the queue. `dequeue_ready` moves into the collect branch body without the `return`, or the branch is restructured as `if (… PARENT_COLLECTS) Scheduler::dequeue_ready(task);` followed by the single queue append — no `return`, no early exit.
- Keep `wake_waiting_parent` result handling and the deferred-`switch` arm structure otherwise unchanged; rewrite the L322-330 / L478-488 comments to state that the publish intent is already queued, the destroy is declined by `kill_declined()` (`wait_collect_pending_`), and the queue entry carries the cause.

**Step 6 — `scheduler.hpp:` contract text + reset seam.** Update `defer_kill` decl (L1355) with the cause parameter and an `@note` "queues the death-publish intent (`task`, `generation`, `cause` as one record); `process_deferred_kills` publishes **after** the AP-live retention check and before the drop/destroy legs"; refresh the stale `defer_kill()` prose (L1901, L1919); make the `publish_task_death` doc-block (L1246) name it the single record-latch choke point with the INV-K1 allowlist; declare `static void reset_deferred_kills_for_test() noexcept;` beside `lockpark_reset_for_test()` (L373) with a doc-block naming it the snapshot/quiesce reset point.

**Step 7 — `src/services/shell.cpp: background_task_wrapper` (L257-263) [F6].** Add exactly one include, `#include <kernel/ipc/death_notify.hpp>` (`DeathCause`); `Scheduler`, `TaskControlBlock`, `arch::hlt`, `arch::sti` are already reachable via `scheduler.hpp`/`task.hpp`/`arch/io.hpp`/`arch/irq_guard.hpp` (`hlt` already used at L262). Then replace the hand-written `state = TERMINATED` + bare hlt spin with the canonical self-exit shape (mirror `_task_trampoline`, `task.cpp:205-221`, and `sys_exit`, `syscall_handlers_misc.cpp:93-121`):
1. `task->state = TaskState::TERMINATED;`
2. `task->exit_code = 0;`
3. `Scheduler::publish_task_death(*task, kernel::ipc::DeathCause::Exit);`
4. `Scheduler::switch_away_from_terminating(*task);`
5. park: `arch::sti();` then a bounded-by-design idle loop with `arch::hlt()` and the trampoline's "parked corpse must never blackout the hart" comment.
Do not return from the function (entry may not be trampoline-wrapped); do not re-order steps 3/4.

**Step 8 — `docs/specs/death-notify.md`.** Add the Step-0 matrix (trimmed to "site → funnel → synchronous?") under Registry Semantics, with row 22 reading "backstop (bulk, suppressed messages)" [F5]; restate INV-K1 (with the `daemon::notify_death` / `DeathNotify::on_task_death` scope split), INV-K3, INV-K5, INV-K8, INV-K9; state the `DeathCause::Watchdog` producer origin; add the PARK/canary/factory-reset non-goals with rationale.

**Step 9 — Gate grep, rewritten with the enumerated allowlist [F2].** Both commands must be pasted into the issue comment.
- `grep -rnE "(daemon::)?notify_death[[:space:]]*\(|on_task_death[[:space:]]*\(" src/kernel/daemon src/kernel/task --include=*.cpp --include=*.hpp` — must return **exactly** these 7 lines and nothing else:
  1. `src/kernel/daemon/daemon_mgr.hpp:74` (declaration)
  2. `src/kernel/daemon/daemon_mgr.cpp:123` (definition)
  3. `src/kernel/daemon/daemon_mgr.cpp:192` (owner self-clear, `restart_stale_daemons`)
  4. `src/kernel/daemon/daemon_mgr.cpp:480` (owner self-clear, admin terminate/reset)
  5. `src/kernel/task/scheduler_terminate.cpp:63` (funnel → PID cell)
  6. `src/kernel/task/scheduler_terminate.cpp:68` (funnel → `DeathNotify::on_task_death`)
  7. `src/kernel/task/task.cpp:3082` (backstop → PID cell; the backstop's record is `publish_task_death` at `:3089`)
  Any additional hit is a REJECT. Test-tree hits (`src/kernel/test/**`) are out of scan scope by path.
- `grep -rn "defer_kill(" src/ --include=*.cpp --include=*.hpp | grep -v "/test/"` must show exactly: the decl, the definition, and 2 production call sites (both KILL arms).

## TEST STRATEGY (QE-owned on `testbed`, stub-first; developer lane touches no test files)

New file `src/kernel/test/test_kill_notify.cpp`, registered via `run_servers_daemon_restart_group()` beside `register_mk_death_tests()` (`test_registry.cpp:510-512`) + `test_expected_counts.hpp` row regenerated with `--class dump-counts`. One `JARVIS_TEST` per row with mandated doc-blocks and `/* Pseudocode */` bodies returning via `JARVIS_TEST_PASS()`:
- `kill_notify_watchdog_arm_synchronous` (cause Watchdog(4) + `DEATH_FLAG_SIGNAL` within 1 tick, parentless helper; extends `watchdog_expiry_kill_default`)
- `kill_notify_parent_collect_retained` (record latched with the **true cause** while the corpse is retained in-table — asserts the Step-4 default; fails if the Q2b hold variant is chosen, in which case the test asserts `Teardown` after collect)
- `kill_notify_ap_live_retained_no_publish` (**INV-K9**: deferred kill of a task still current on an AP → zero record + zero PID-cell clear on the first pass; exactly one record with the carried cause after deschedule)
- `kill_notify_retain_cause_coherence` (no cause/task cross-wire after compaction; multi-entry retain + drop mix)
- `kill_notify_trap_funnel_cell` (arch-neutral `terminate_err_from_trap` → cell cleared, cause Fault; all three archs)
- `kill_notify_real_trap_x86` (real Ring-3 fault via the per-arch fault pattern, x86-gated)
- **`kill_notify_real_trap_el0_aa64`** [F7] — positive EL0 fault kill on aarch64: stage the existing per-arch fault vehicle (`test_debug_syscall.cpp:1187-1213`: `kBrkInsn` stub poison / `kUnmappedFaultVa` instruction-abort redirect, control leg already proves `TERMINATED`), then assert the record + PID-cell clear + cause Fault through the aa64 funnel `arch/aarch64/interrupt_controller.cpp:504-509`.
- **`kill_notify_real_trap_umode_rv64`** [F7] — positive U-mode fault kill on riscv64: same vehicle with `kFaultInsn` `c.unimp` at the stub, asserting record + cell + cause Fault through `arch/riscv64/interrupt_controller.cpp:292-296`.
- `kill_notify_shell_background_exit` (cause Exit, zero PMM/Tasks delta, no hlt-spin leak)
- `kill_notify_no_supervisor_noop` (INV-K6: recv 0, no block, no error)
- `kill_notify_double_publish_once` (INV-K4: exactly one record)
- `kill_notify_recycled_supervisor_no_poke` (INV-K7: no poke on gen-reused supervisor id)
Fixtures: pre-death ids only; `need_resched + hlt` waits; tick-frozen timing under `IrqGuard`; relative tick bounds only. No negative-evidence-only arch leg: every arch-visible trap claim is positive on its own arch (aa64/rv64 legs above) and the arch-neutral funnel cell stays as the cross-arch constant.
Isolation deliverable [F4]: in `test_isolate.cpp` `snapshot_restore()` call `Scheduler::reset_deferred_kills_for_test()` in the pre-MemPool-restore block immediately after `Scheduler::reset_migration_counts()` (`:987`), i.e. after `Scheduler::drain_zombie_list()` (`:968`) and before the pool refcount rewind — IF=0 by the restore invariant asserted at `:886`; count + all three arrays zero, never published. Evidence: any deferred-kill test followed by a `servers_daemon_restart` run with zero spurious `[DMD] Task … killed` lines and zero ResourceTracker delta.
Run order: `servers_daemon_restart` → `core_task` → `ipc` → `deadline` → `capability` → `servers` → `proc_elf` → `ui` → `arch_aarch64` → `arch_riscv64`; one `test-history.txt` row per run.

## SIL 3 / SAFETY RISKS

- **[S1] Publishing for a task still executing on an AP** → supervisor starts a second instance over a running corpse. Mitigate: **AP-live retention first, publish second** (INV-K9); retained entry keeps its cause and announces on the pass after deschedule; `kill_notify_ap_live_retained_no_publish` is the gate.
- **[S1] Earlier reply-waiter wake perturbs a BLOCKED-window handshake** (#559 class) for parent-collect corpses now announced at the decision. Mitigate: same-`on_tick`-tail ordering for non-retained victims; `ipc` + `servers` green before/after; revert-on-S1/S2.
- **[S1] Stale `death_cause` on a recycled TCB** suppresses the latch. Mitigate: verify the **five** memset sites + the `task.hpp:392` init-list zero ([F3]); CONFIG_DEBUG-only assert in `note_death_cause` (diagnostic only).
- **[S2] Cause/task cross-wire across append/retain/drop** → wrong cause or record for the wrong task. Mitigate: INV-K8 single-index three-slot rule, count-as-commit-word, no-subset copies, full-list leg writes nothing, coherence test.
- **[S2] Stale deferred intent surviving test isolation** → a kill fires against a restored-baseline task. Mitigate: `reset_deferred_kills_for_test()` at the documented quiesce/IF=0 point before the MemPool rewind; drop-not-publish.
- **[S2] Single-choke invariant mis-scoped / gate false-negative** — forbidding the daemon manager's own registry clear, and a qualified-only grep missing unqualified callers. Mitigate: INV-K1 scope rewritten to the record latch, Step-9 gate matches qualified **and** unqualified `notify_death` plus `on_task_death` with the 7-line enumerated allowlist ([F2]).
- **[S2] Shell wrapper double-terminates or returns into a null frame.** Mitigate: explicit funnel ordering (`publish` then `switch_away`), non-returning sti/hlt park, `ui` + `servers` shell tests, the one added include ([F6]).
- **[S2] Future caller publishes under `scheduler_lock_`.** Mitigate: entry ENSURE + doc-block take-site discipline.
- **[S3] `DeathCause::Watchdog` gains its first producer** — enum comment + spec table change in the same diff; no new dmesg codes.
- **[S3] PARK dispositions produce no record** — deliberate non-goal N2.
- **[S3] `CONFIG_DEADLINE_ACTION` defaults 0** — arm #19 compiled out; verified-by-inspection + shared-`defer_kill` coverage, no fabricated gate.

## OPEN QUESTIONS (owner decisions required before implementation)

1. **Deadline arm cause value** — `DeathCause::Kill` (3, recommended: budget overrun is a policy kill) vs `DeathCause::Watchdog` (4). Supervisor-visible ABI semantics.
2. **Q2b — parent-collect-retained (non-AP-live) announce timing** — latch at the decision with the true cause (recommended default: closes cause fidelity, corpse is provably dead and off-CPU) vs hold until teardown completes (cause degrades to `Teardown`, announce bounded by the parent). The AP-live half of Q2 is **settled**: publish at decision only when non-AP-live, retained entry announces on the later pass.

## NON-GOALS

- **N1** Factory/boot reset bulk kill (`taskdefs.cpp:712-719`) — runs through the `Teardown` backstop (row 22), with death messages suppressed (`:723`) and `daemon::reset_clear_daemons()` immediately after (`:727`); arming records on this boot path would produce spurious restarts. Conclusion unchanged, matrix wording corrected ([F5]).
- **N2** `trap_owner_trip` PARK dispositions — fail-closed park, no attributable `dead_id`.
- **N3** Canary validators — fail-stop panic policy; conversion is out of scope.
- **N4** Pinned-TCB teardown skip + `cleanup_test_tasks` — test-only paths.
- **N5** `DeathNotify` registry/ABI semantics, SYS_DEATH_WATCH/RECV/UNWATCH, watch authority — untouched.
- **N6** No new locks, no new `reschedule()`, no arch-specific code, no `terminate_err` sequencing change, no `daemon_mgr` restart-policy change.
- **N7** `debug::debug_publish_death` — separate channel, already in `cleanup()`.

## VERIFICATION GATES

1. `make check-style` → Errors: 0; `tools/meson/build.sh` clean.
2. 3-arch debug link green, same cycle.
3. Per-class runs in order: `servers_daemon_restart` → `core_task` → `ipc` → `deadline` → `capability` → `servers` → `ui` → `arch_aarch64` → `arch_riscv64` (`proc_elf` per `test-policy.tsv:63`), with the aa64/rv64 positive trap legs executed on their own arch.
4. Full 2×3 matrix `bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} {debug,release}` before merge.
5. Evidence: one `test-history.txt` row per run; issue comment with counts + the rewritten Step-9 grep output (7-line allowlist exact match); audit report + DECISION on #516.
