# Design review — issue #582 (scheduler strand: suspend-while-holding-`scheduler_lock_` (B) + ready-queue link/flag desync (A))

Scope: plan only (no diff exists). Read-only audit against main. Checked: `global_state.cpp` (validate/drop_arm 768-913), `scheduler_lock_guard.hpp` (mark-after-take 66-70, unmark-before-release 81-89), `spinlock.hpp` (`is_locked_for_debug` 102-104), `isr_stubs.asm` (.depth_ok 236, pre-save/gen-check 238-304, validate call 322-340), `vectors.S` (`irq_context_switch_common` 302-319), `syscall_entry.S` (gen lock 269-310, validate 321-328, re-derive 330-343), `scheduler_switch.cpp` (direct clears 280/282/486/701, `try_direct_switch` 1120-1139, `switch_away_from_terminating` 940-1103), `scheduler_tick.cpp` (park gates 1255-1329, park body 1336-1372, `reschedule` 1375-1490), `scheduler_queue.cpp` (`enqueue_ready` 264-325), `task_queue.{hpp,cpp}`, `ready_queue_manager.hpp`, `park_lock.hpp`, `irq_guard.hpp`.

## FINDINGS

- [S1] INV-APPLY-3 (force outranks lock-hold deny) reintroduces defect B — `WHY:` a terminate-driven arm with `force_apply=1` applied over an interrupted lock-holder suspends it mid-critical-section (the exact B freeze); latching `[S582-FORCE-HOLD]` reports the freeze without preventing it, and the plan's deferral gate is bypassed on precisely the path that must be gated.
- [S1] INV-PARK-2 "denial self-heals / non-freezing" is false as specified — `WHY:` current `reschedule()` parks only when `resched_park_candidate != null` (tick.cpp:1440-1441, 1487-1489), so a bit-6/SchedLock-denied BLOCKED caller runs on while BLOCKED holding `scheduler_lock_` with no re-park loop in the plan, recreating the F1 zombie window (tick.cpp:1234-1240) the park was built to close.
- [S2] Per-arch gate call clobber/offset discipline unspecified — `WHY:` x86 must preserve `r11` (per-CPU index, caller-saved) across the gate call and define `rax`-return interplay with the later generation/validate sequence; aarch64 must reload both `x0` (slot addr) and `x1` (`sp`) after the `bl` (both caller-saved, clobbered) before `str x1,[x0]`; riscv must reload `t0` (slot addr) plus re-derive `t2` and stash `ra` before the gate `call`, none of which the plan states.
- [S2] riscv force-consume vs gate ordering undefined — `WHY:` `syscall_entry.S:244-248` consumes `scheduler_force_apply` before `check_depth`, so a gate inserted after line 304 either loses the force (consumed then denied) or, if force outranks, re-applies under hold (S1 above); the plan must specify deny-and-rearm vs deny-and-drop semantics.
- [S2] aarch64 torn-publish hole not closed by the gate — `WHY:` `vectors.S:299` documents no generation re-check on aa64 ("tracked follow-up"), so the new apply gate defers lock-holders but still applies half-written RSP/CR3 pairs, leaving one arch with weaker atomicity than the other two.
- [S2] Bounded scan / restore-drop / refused-enqueue fail-closed behavior undefined — `WHY:` a hint-miss scan that hits its bound without finding the node, a `restore_level` validate-or-drop of a READY task, and a refused enqueue each risk leaving READY+unqueued (the A strand) unless the plan states the exact state repair or escalation for each miss path.
- [S2] A-funnel attribution lacks a per-path window table — `WHY:` the ENQ-last-op/no-unlink-note evidence is consistent with `push_back` flag-then-link preemption, but the plan must enumerate every uninstrumented writer (`set_raw`/`restore_pod`/`reset`/`clear_all`/EDF insert-remove/move/`rebuild_ready_queue`/terminate paths, `ready_deferred_` exception interplay) with its IRQ/lock window before the single-discipline rewrite can claim completeness.
- [S2] GATE 2 "proceed on invariant argument alone" fallback unacceptable — `WHY:` a SIL 3 fix for a six-wedge hunt cannot be accepted without repro or fault-injection evidence; B-before-A ordering is correct, but the fallback lowers the evidence bar below the audit standard.
- [S3] Deferral-streak latch has no escalation — `WHY:` streak > 8 proves the holder is stuck, yet the plan only latches a diagnostic, so an unbounded deferral starves the armed target silently.
- [S3] Counter/isolation-reset wiring must be QE-owned and explicit — `WHY:` `apply_defer_count`/`streak_max`/new dmesg SCHED+16 row need snapshot-restore reset and `test_expected_counts` rows on testbed, which the plan correctly excludes from dev scope but should name as acceptance prerequisites.

## Verified sound (no finding)

- INV-APPLY-1 conjunction: `SchedulerLockGuard` marks depth after `lock()` (66-70) and clears before `unlock()` (81-89), so both ctor/dtor windows present `locked_=1`; `depth_own()==0 && !locked` therefore denies in every hold window — the conjunction argument holds, failing closed toward defer.
- Gate fast path (two relaxed loads + per-CPU index) is ISR-safe; existing precedent (`scheduler_validate_pending_switch`, `[H2-ABORT]` unconditional `raw_write`) covers the one-shot diagnostic lines.
- `TaskQueue` IRQ-atomicity via `IrqGuard` nesting is compatible with T2b (guard nesting is legal; the no-reentrant-take rule constrains `scheduler_lock_`, not `IrqGuard`).
- Scope split (dev production files, QE test/count/isolation files) respected.

## Required changes before implementation

1. INV-APPLY-3: force must not outrank the lock-hold deny (deny-and-rearm or equivalent), with the rv64 consume/gate ordering specified.
2. INV-PARK-2: specify the post-denial mechanism (re-park loop after unlock or proof the state is unreachable) reconciling the F1 tension explicitly.
3. Per-arch call-site specification: preserved/reloaded registers, slot-index derivation inside the C gate, and aa64 generation-lock disposition.
4. Fail-closed table for defer/deny/hint-miss/restore-drop/refused-enqueue misses; per-path A window table; GATE 2 evidence bar without the argument-alone fallback.

`DECISION: REJECTED`
