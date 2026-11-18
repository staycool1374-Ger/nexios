# Design re-review (rev 2) — issue #582

Scope: plan only. Read-only on main. Checked: `scheduler_lock_guard.hpp` (66-99), `task_queue.cpp` (9-27, 59-70, 77-111), `ready_queue_manager.cpp` (18-50, 147-216), `scheduler_tick.cpp` (70-167 adopted guards, 560-620 sweep, 1240-1490 park/reschedule), `scheduler_switch.cpp` (940-1103 terminate, 1120-1139 direct), `scheduler_queue.cpp` (264-358), `spinlock.hpp` (83-90), `scheduler.hpp:1271`, `vectors.S:270-349`, `syscall_entry.S:230-348`, `isr_stubs` x86_64 `:236-343`, `global_state.cpp:768-820` drop_arm, `park_lock.hpp:38-52`.

## FINDINGS

- [S1] INV-PARK-2 re-park hook fires in ISR context — `WHY:` `~SchedulerLockGuard` is the hook site, but `on_tick` holds adopted guards in timer-ISR context (`scheduler_tick.cpp:86,105,167`); the stated park condition ("still BLOCKED and current") has no ISR/IF re-check, so a latched denial serviced at an ISR-context release runs the sti/hlt park body inside the ISR.
- [S1] INV-PARK-2 early-`unlock()` bypasses the hook — `WHY:` the plan hooks only the destructor (`scheduler_lock_guard.hpp:81-90`), while `unlock()` (`:94-99`) releases without servicing the latch, leaving the exact F1 zombie (BLOCKED + runs on) on every early-release path.
- [S1] NEW dead-owner recovery is unimplementable on main's primitives — `WHY:` `scheduler_lock_` is `sync::SpinLock` (`scheduler.hpp:1271`) whose `holder_` is a return address (`spinlock.hpp:86-87`) with no TCB-owner identity, so L1 "verify owner==&exiting" / L3 "dead-owner shape" cannot distinguish a dead holder from a legal live preempted holder (`scheduler_tick.cpp:1379-1380`); worse, recovery sits behind the blocking guard take (`scheduler_switch.cpp:947`), so with a truly dead holder the terminate path spins before recovery is reachable.
- [S3] Abort-64 reuse is sound but "symmetric restore" is misstated — `WHY:` `drop_arm` skips the drop on APs (`global_state.cpp:792-793`) and streak reset across abort is unstated (instant re-abort vs respam depends on it).
- [S3] Force lifecycle has a stale window — `WHY:` "cleared at apply" covers no abort/drop/cancel path, so a denied-then-aborted force bit keeps the rv64 force-jump bypassing `check_depth` until the first passing apply (gen+lock gates still hold, so B-safe).
- [S3] R5 row-2 prime is non-discriminating; EDF scoping is tense — `WHY:` ENQ-last-op/no-unlink-note fits a refused re-enqueue on a stale flag (the sweep at `scheduler_tick.cpp:592-594` names exactly this) as well as an ISR-pop window, and "every mutation via TaskQueue" sits uneasily with "EDF lists separate with own tags" — GATE 2(ii) proves possibility, not the racing path.

## Disposition per original finding

1. S1 INV-APPLY-3 (force outranks deny): original defect CLOSED — L2 deny-and-rearm + L3 ignore-force/live-holder-deny + rv64 peek/ordering is deny-absolute. New recovery sub-mechanism REJECTED (new S1 above).
2. S1 INV-PARK-2 (denial self-heals): OPEN — latch+post-release hook is the right shape, but ISR-context firing and `unlock()` bypass are unspecified.
3. S2 x86 call-site discipline: CLOSED — r11 preserved, rax reloaded (`:300-301`), rcx post-gate capture, rbx not live at gate point, IF=0 throughout; matches `.depth_ok/:236-242` constraints.
4. S2 rv64 force/gate ordering: CLOSED — peek-no-consume at `:244-248`, gate `:304-310` with ra stash + t0/t2/t3 reload rules, deny via `gen_skip` leaving force set, clear only after gate+gen pass.
5. S2 aa64 torn-publish: CLOSED — x4 capture/re-check pair fits the span (only x0-x3 live; x5 first live at `:347`), DAIF-masked premise already documented (`vectors.S:296-299`), mismatch→`.restore` preserves the arm like x86 `gen_skip`.
6. S2 fail-closed misses: CLOSED — rows 3/4/5/6 name repair+escalation, and the repair net exists and is #582-aware: per-tick stranded sweep under lock (`scheduler_tick.cpp:560-620`, bounded scan, both flag shapes rescued + counted).
7. S2 A-funnel table: CLOSED — 17-row table delivered as demanded; fix (typed IRQ-atomic ops + sweep net) is window-agnostic so row-2 misattribution would not break safety.
8. S2 GATE-2 argument-alone fallback: CLOSED — fallback removed, A2 requires repro (i) or injection (ii).
9. S3 streak escalation: CLOSED — rows 1-2 + SCHED+18 + abort-64 specified.
10. S3 QE/counter wiring: CLOSED — R7 merge gates name isolation resets, dmesg rows, expected counts.

`DECISION: REJECTED`
