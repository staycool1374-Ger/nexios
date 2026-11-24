# AUDIT REPORT 2026-11-24-design-review-607-r1 (SIL 3 DESIGN review, issue #607)

PLAN: audits/plan-607.md (design-only audit; no code diff exists yet)
BASE: main @ 4daa93ea (verified `git log --oneline -1`; branch `main`; tree untouched, nothing committed)
NORMATIVE ISSUE: #607 ([S2][process] sys_waitpid publish-side window; follow-up to #590)

## SCOPE

Design review of the plan to fence `ProcSvc::waitpid`'s publication sequence
(`waiting_child_status → dequeue_ready → BLOCKED → waiting_child_pid → SEQ_CST fence → DEBUG fire`,
`src/kernel/proc/proc_svc.cpp:304-321`) under one production `arch::IrqGuard`
(L1), with the existing #590 `fire_wait_publish` entry guard nesting inside it (L2).
Every load-bearing claim below was verified against the tree at the pinned base.
PROMPT-audit critical checks applied as adapted for design (no patch to re-verify
by execution; static verification only).

## VERIFICATION (the 8 load-bearing checks)

1. **Premise correction — CONFIRMED.** `arch/x86_64/hal/idt.cpp:69-71`: SYSCALL
   vector uses gate `0xEE` (interrupt gate, D1=3, clears IF → IF=0 on user-trap
   entry). `docs/specs/irq-preemption-contract.md:9` (entry contract) and `:13`
   (no dependence on entry-IF except by declaration) verified verbatim.
   Kernel-task callers: `test_waitpid.cpp:55-59` calls `Syscall::handle(WAITPID)`
   directly from task context; `src/kernel/syscall/syscall.cpp` contains zero
   `sti()/cli()` and zero `IrqGuard` → IF=1 on that path (tests assert
   `interrupts_enabled()`). PROC_MGR=1: `proc_svc.cpp:213-214` →
   `proc_mgr_call` (`proc_mgr_proxy.cpp:181-221`) → `IPC::send_sync_bounded_err`
   (`ipc.cpp:1163+`, returns with `arch::sti()` on reply/timeout legs) → IF=1
   at the window. Default `CONFIG_PROC_MGR=0` confirmed (`nexios_config.h`,
   "must be 0 or 1 (default 0)"). aarch64 (`vectors.S`, `daifset`) and riscv64
   (`syscall_entry.S:62`, `csrci sstatus,2` first) trap-masking confirmed.
   Only exposure-shape correction: none material (see S3-1/S3-2 nits).
2. **Guard topology / no-stray-sti — AIRTIGHT, confirmed against
   `irq_guard.hpp:135-189`.** Ctor saves `irq_was_`; dtor stis iff `irq_was_`.
   L1 saves caller IF (both values correct); L2 is constructed with IF=0 so
   `irq_was_=false` → L2 dtor can never emit `sti` in any of the 4 entry/ordering
   combinations. Depth `0→1→2→1→0` holds (no return/break inside the window;
   fire hook is void). Owner token set on the 0→1 transition only; nothing in
   the window consults an owner/depth-keyed gate. Keeping L2 is the right call:
   zero release bytes (inside existing `#if defined(CONFIG_DEBUG)`), seam keeps
   its own contract, no new reentrancy surface (L2 takes no lock, emits no sti,
   touches depth/owner/trace only). No merge (moving L1 into the fire site would
   make the fix DEBUG-only).
3. **Boundedness inventory — CONFIRMED complete.** `dequeue_ready`
   (`scheduler_queue.cpp:496-514`): `validate_task_ptr` (O(1), raw-write logs
   only on corruption, `scheduler_integrity.cpp:146-172`), conditional
   `edf_remove` (own nested guard, no lock, `:108-124`), `rq.remove`
   (`ready_queue_manager.cpp:121-174`, no lock member; hint `try_unlink` + bounded
   scan `q=0..CONFIG_PRIORITY_CEILING`, `CONFIG_PRIORITY_CEILING=127` per
   `docs/specs/configuration.md:56` → 128), `invalidate_pending_switch_to`
   (`:290-300` → `cancel_pending_switch_cpu`, `scheduler_smp.cpp:39-62`: 7 atomic
   stores + generation bump per CPU, loop `CONFIG_MAX_CPUS=8`, no IPI/lock/sti;
   the `SchedulerLockGuard` at `scheduler_smp.cpp:117` belongs to
   `mailbox_drain`, not this path). `SchedulerLockGuard` sites in
   `scheduler_queue.cpp:551/570/598` belong to `set_sched_policy`/
   `set_edf_exempt`/`assign_deadline_priority`, not the window. `scheduler_lock_`
   textually unreachable on the window path (undo-leg `set_task_ready` at
   `proc_svc.cpp:348` is outside the scope). `zombie_lock_` reachable only via
   the DEBUG hook funnel (`release_zombie`, `SpinLockGuard` takes in
   `scheduler_lifecycle.cpp:103/122/156/247/347`), UP-gated at arm
   (`:537-538`, `up_cpu_count()!=1` refuses). Zero `sti` verified in all six
   plan-listed TUs **plus** `sporadic_server.cpp` (reachable via
   `effective_priority` → `current_priority()` const accessor; no sti/lock/log —
   see S3-3). No alloc/IPC/reschedule on the success path; cold legs disclosed
   (forced-clear one-shot `Logger::error`, DEBUG-only `sched_runq_trace` which
   compiles to empty inline in release, `scheduler_debug_internal.hpp:89-95`).
   `effective_priority` nested guard only (ledger S1).
4. **Release quantification — CREDIBLE.** 129 = 1 hint + 128 scan attempts, each
   an O(1) `try_unlink` with its own nested guard; ≈3-5 kcycles ⇒ ≈1.5-2.5 µs
   @2 GHz is a sound order-of-magnitude estimate for TCG/silicon (unmeasured —
   acceptable at design; plan's optional one-shot rdtsc check endorsed).
   Trap path masked-delta is exactly 0 ns (`irq_was_=false`, no `sti`).
   0.15-0.25% of the 1 ms tick (`CONFIG_TICK_HZ=1000`); watchdog grace is 500
   ticks — no timing gate is at risk. Not-new-work precedents verified: ledger
   S6 (`terminate`) and the `block_sender` publish guard (`ipc.cpp:1595-1637`,
   which spans BLOCKED+dequeue under IrqGuard). RQ fast path correctly ruled out
   per INV-RQ-1/2 (`task_queue.cpp:126-135`, links-authoritative).
5. **INV-WAIT-4 + fence pair — PRESERVED, re-verify NOT dead.** Statements, order
   and both SEQ_CST fences untouched; guard adds masking only. The re-verify
   (`:334-349`, `kWaitReverifyRetryMax=4`, `:189-193`) stays live for (a) the
   unmasked gap between guard-drop and the `:334` acquire-load, and (b) the full
   cross-CPU path, which the per-CPU mask never closed. SMP repair correctly
   stays outside the guard (see INV-PUB-IRQ-7 brace placement; forbidden
   extension over `:334-404` correctly specified).
6. **QE spec — DISCRIMINATOR CORRECT, one prescription missing (S3-5).**
   Pre-fix seam state: IF==0 via L2 alone at depth==1; post-fix depth==2 —
   verified no other guard is live on the test path (`syscall.cpp` guard-free,
   test calls `Syscall::handle` inline with IF=1). `depth>=2` is the right
   predicate; IF==0 alone is correctly rejected as non-discriminating. Optional
   named test is sound. Tick-in-window effect test correctly deemed
   unconstructible; SMP supplement correctly deferred (stub `smp_waitpid`,
   `test_smp_waitpid.cpp:47-49`, counts `:129`, must not be cited as evidence —
   plan says so explicitly).
7. **Gates — ADEQUATE.** 3-arch builds + release build; `process_waitpid` 11/11
   matches `test_expected_counts.hpp:87`; 3× identical-binary rerun; adjacent
   classes incl. `ipc_core`/`ipc_blocking`; off-arch legs (`arch_scope|any`
   default confirmed, only `smp_waitpid` is arch-restricted per
   `tools/test-policy.tsv:43`); release 1/1 leg matches `REGISTER_RELEASE_TEST`
   (`test_waitpid.cpp:1554`); selftest last; `check-style` Errors: 0; one
   history row per run, append-only. The brief's `all`-class staleness
   conditional does NOT trigger: the plan names no `--class all` gate, and
   `all` was removed from the harness (`scripts/run_all_classes.sh:2`). The
   cited `all TIMEOUT` (09:13:14) and `arch_aarch64` 37/1 (09:17:00+) history
   rows are real and the plan correctly demands accounting (identify or file),
   never dismissal.
8. **Check-7 retrieval — REAL.** 10 rows with queries, results and per-row
   dispositions (used/ignored + why); cited symbols (`sys_waitpid`,
   `dequeue_ready`, `wake_waiting_parent`, `waiting_child_pid`) match the tree
   (the `waiting_child_pid` no-node negative result is consistent with a field,
   not a symbol). "Open questions: none" is credible for the design itself;
   the four deferred follow-ups are documented in the SIL 3 section — but only
   as prose (see S3-6).

## FINDINGS

- [S3] plan-607.md:88-101 vs tree — terminology/citation nits, WHY: `0xEE` is an
  *interrupt* gate (clears IF); "trap-gate accident" phrasing is loose though the
  table's "interrupt gate 0xEE" is correct. The PROC_MGR `sti` chain should cite
  `send_sync_bounded_err`'s return-`sti` legs (`ipc.cpp:1163+`), not only
  `recv`'s `:1473-1478/:1507`; and `CONFIG_PROC_MGR` default lives at
  `nexios_config.h:1664-1665`, not `:1771`. Substance verified unaffected.
- [S3] plan-607.md INV-PUB-IRQ-2 item 8 — inventory omits one reachable TU, WHY:
  `effective_priority` → `SporadicServer::current_priority()` pulls
  `sporadic_server.cpp` into the window; audited clean here (no sti/lock/log —
  const accessor), but the plan's grep-evidence list should name it so a future
  edit there gets re-audited against this window.
- [S3] plan-607.md INV-PUB-IRQ-6 — name the `:321→:334` unmasked gap explicitly,
  WHY: it (plus the cross-CPU path) is the positive reason the post-fence
  re-verify is live code rather than dead-but-present; stating it pre-empts the
  exact dead-code challenge the plan anticipates.
- [S3] plan-607.md TEST STRATEGY — add a pre-fix control leg, WHY: the extended
  `seam_observe_only` pin (IF==0 AND depth≥2) must be run once against the
  pre-fix tree and observed to FAIL (depth==1), otherwise the pin's
  discriminating power is asserted, not demonstrated. (Note: EDF members will
  transit through depth 3 via `edf_remove`; `≥2` already accommodates this.)
- [S3] plan-607.md SIL 3 risks — file the four deferred follow-ups as numbered
  issues (RQ hint-miss scan cost; ledger re-enumeration sweep; kernel-caller
  −1-after-publish at `:390`; `RqmRemoveHintMiss` DEBUG flood) and post the
  numbers on #607 with the fix commit, WHY: "file a follow-up" prose without
  numbers is how S3s evaporate; the plan is otherwise exemplary at naming owners.

## PATCH

Omitted (design review — no code diff; findings travel as plan amendments above).

DECISION: APPROVED
