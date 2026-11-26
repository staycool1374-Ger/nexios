# PLAN-546: Preemption/IRQ/liveness contract — residual closures in four landable increments

**Issue:** #546 (S2, sil3-relevant, kernel:scheduler). **Scope note:** the issue text describes the state before M2. The tree today already contains most of M2 (contract doc, IrqGuard depth, ParkLockGuard, lock ranks, INV-C1, TSC tick watchdog) plus later hardening (#582 park latch, #598 victim-keyed breaker, #600/#601/#602 oracles). This is a residual plan: (a) census of what is landed, (b) the four named pieces partially landed with structural holes, (c) closures only. Nothing re-does M2.

## 0. RETRIEVAL ARTIFACTS & STATE CENSUS

| Query | Result | Disposition |
|---|---|---|
| graphify query_graph("IRQ contract, spinlock park discipline, scheduler liveness, TSC tick watchdog") | 476 nodes; cluster = scheduler.hpp, park_lock.cpp, irq_guard.hpp, spinlock_guard.hpp, PriorityMap, ReadyQueueManager, is_test_active(). No node for a liveness/progress ledger, no node for IRQ-off duration/WCET. | used: park/price/rank cluster is the integration surface; no progress-guarantee mechanism exists (increment D genuinely new). |
| graphify get_node("irq_guard_depth_own()") | src/kernel/arch/hal/irq_guard.hpp:105, degree 1 | used: depth counter is observational, not asserted at block points (increment A). |
| obsidian search_vault("irq-preemption") / ("liveness") / ("546") | audits/design-review-561-r1.md:13, design-review-583-r1/code-r1, audits/audit-task-sync-v0.4.2.md:53 | used: rank-table incompleteness is known-deferred (M8); #583 park-gate fork precedent — no second source of truth. |
| docs/irqguard-ledger.md, docs/specs/schedoracles.md | 136 production guard sites; schedoracles single-family readout/reset discipline | used as mandatory shape for all new counter families. |

Census holes: (1) INV-C1 debug-only → release never checks; no IF/context asserts at block points or syscall exit; no IRQ-off duration measurement. (2) SpinLock::lock() unbounded; holder-never-blocks unenforced; rank table covers 5 locks, scheduler_lock_ invisible. (3) EDF stand-down disables breaker; guarantee is band-level; lock waits unbounded/undetectable. (4) Watchdog needs checkpoints for coverage; ACTION 0 forever; grace 500 guessed; domain overclaimed off-x86; AP tick path never calls note_tick().

## 1. INCREMENT A — IRQ contract: stated → enforced (no new locks, no new ABI)

New `src/kernel/sync/irq_contract.{hpp,cpp}` (ExecCtx classifier, check entry points, violation counters, IRQ-off lateness high-water, test-isolation reset). `irq_guard.hpp` outer-transition timestamps (sampled). `on_tick` entry lateness sampling (always-on, 1 counter read). INV-C1 de-gated from CONFIG_DEBUG (policy `CONFIG_IRQ_CONTRACT_ASSERT` 0/1/2, identical both builds). `Scheduler::note_block_transition(site, self)` after each of 40 BLOCKED stores (post-store). `syscall.cpp` real exit assertion + rename mislabeled entry checkpoints. `park_lock.cpp` park-entry context assertion + escalation bookkeeping (no bound introduced). Docs: contract §1/§3/§8 + ledger assert-coverage column. New config keys with `#ifndef` + value `#error` pairs.

Cross-arch: shared C++ on `arch::interrupts_enabled/irq_guard_depth_own/isr_nesting_own/in_syscall_trap_level` + `arch::Timer::raw_counter/raw_counter_freq_hz` (all three arches). x86 gates sampling on `calibrate_tsc` having run. Doc caveat (not deferral): aa64/rv64 sample duration, not independent-domain (increment C owns independence).

QE stubs (testbed): ctx_classify, block_self_clean/guard_live, syscall_exit_restores_if, lateness_highwater, policy_parity (release build asserts compiled), reset_isolation.

Gates: synchronization_irq_guard → synchronization_spinlock → synchronization_lock_order → scheduler_core → core_sync → core_sched → ipc → core_syscall → selftest; aarch64/riscv64 link + arch classes; debug+release x86_64; check-style 0; test-history rows.

S1 risks: INV-C1 de-gate changes release failure policy (mitigate: land de-gate first, log-only matrix with zero SCHED+15 hits before default flip); false-positive context assert on legal sti-owning waits (mitigate: ExecCtx classification + DeclaredWaitScope exemption). S2: sampled dwell cost (default off release, factor 64 debug, identical-binary timing); 40-site self/foreign misclassification (reviewable ledger table first). S3: checkpoint tag renames.

Non-goals: no new block rules, no independent probe, no SpinLock conversions, no WCET ratchet.

## 2. INCREMENT B — Spinlock discipline: one story, enforced, complete (no new locks on kill/IRQ paths)

`SpinLock::lock()` → bounded spin + checkpoint + `sync::spin_contended_hook` (weak default = status quo; strong scheduler-TU definition parks only when allowed). Rank every nesting-relevant lock; ledger rows for Unranked-with-reason. `scheduler_lock_` take visible as rank Sched(1) (depth authority unchanged). Outermost task-context takes → ParkLockGuard (queue/notify/eventgroup/semaphore/mutex outermost lists); inner re-takes stay SpinLockGuard. Exhaustion-path holder probe (read-only, config-gated). Contract §2/§4 INV-LOCK-1..4 + site table; IrqSpinLockGuard classification.

Cross-arch: arch::pause() all three; hook strong definition shared; re-verify INV-L1' IF-restore on aa64/rv64 (new caller class); gates include arch classes + hal_core/hal_timer.

QE stubs: bounded_rounds, park_from_primitive, inner_no_park (LockHeld deny), holder_blocked_detected, rank_table_complete, rank_order_sched_first, reset_isolation.

Gates: synchronization_lock_order → synchronization_sync → synchronization_locking → core_sync → scheduler_core → core_sched → ipc → ipc_core → drivers_core/virtio → selftest; 3-arch + check-style.

S1: park inside callers assuming non-blocking (mitigate: hook no-op unless park-allowed, negative tests, per-arch sti/hlt audit, two-commit revert path); wrong ranks (report-only, ledger authority). S2: order-stack depth overflow (saturating push + counter + oracle); holder-probe cost (exhaustion-only, lock-free). S3: park-count re-baselines in QE change.

Non-goals: no PI for spinlocks; no ISR/device conversion; inversions stay report-only (M8 owns enforcement); no SMP.

## 3. INCREMENT C — TSC watchdog: coverage, fail-safe, honest domain (after B)

Grace from measurement (`max(floor, 10× measured_max_lateness)`, source published). Per-CPU note snapshots; AP path fail-closed skip (UP-only, SMP milestone owns semantics). Coverage ledger doc + missing checkpoint() calls (§7 audit list; no fast-path checkpoint in SpinLock::lock, every-256 only). Policy: ACTION 0 default, ACTION 2 reachable-but-not-default. Readout family + reset (schedoracles shape). Doc: §5 per-arch domain honesty (x86 independent; aa64/rv64 same-counter = tick-flow supervision), §8 ratchet table.

Cross-arch: shared code; per-arch domain table in doc; rv64 mtime dependency already relied upon; try_enable treats freq 0 as disabled; arch classes exercise freeze/advance.

QE stubs: grace_from_measurement, coverage_ledger_present, ap_path_fail_closed, action_policy_pinned, raw_counter_advances_while_frozen (per-arch), reset_isolation.

Gates: scheduler_core → core_sched → core_sync → arch_aarch64 → arch_riscv64 → hal_timer → configuration_build → selftest → full matrix + check-style.

S1: grace auto-shrink false fire (mitigate: monotone high-water, floor, 10×, ACTION 0 shipped); wiring ACTION=2 (not default, separate audited change). S2: ledger formally complete but behaviorally unproven (mitigate: QE soak must show FIRE on injected wedge with site tag); per-CPU arrays on UP (static_assert, documented). S3: serial line length discipline.

Non-goals: no NMI/probes; no SMP tick flow; no frozen-counter detection.

## 4. INCREMENT D — Liveness as positive guarantee (last; consumes A+B+C)

New liveness_ledger.{hpp,cpp} (per-task ready age, per-band denial age, wait ceilings, readout + reset; NO TCB growth — per-CPU side table, owner decision OD-7). Age accounting inside existing on_tick walk (749-854). EDF breaker extension (relief tick 1-in-N, same #598 ceiling). Wait-ceiling escalation (count/log/panic by config; wait continues). Escalation single function, identical both builds. Reset/restore wiring. Docs: scheduler.md R5/R6, liveness.md new, contract §4 ceiling.

Cross-arch: shared tick-level; forced pick through same switch path all three arches; arch epilogue assumptions covered by arch_aarch64/arch_riscv64/scheduler_preemption/scheduler_core gates.

QE stubs: ready_age_zero_healthy, two_spinners_lower_progress, edf_hammer_lower_relief (pre-fix: timeout/zero relief), wait_ceiling_reached, holder_blocked_observed, reset_isolation.

Gates: scheduler_starvation → scheduler_core → core_sched → core_sync → ipc → arch classes → selftest → full 3-arch matrix + soak 0 FIRE on quiet host + check-style.

S1: forced dispatch violates EDF deadlines (mitigate: 1-in-N ceiling, counted, documented trade-off; OD-5); hot-loop/snapshot cost (no TCB growth, derived-state-only rule, identical-binary timing). S2: ceiling false fire on legitimate long waits (windows not wall time, ACTION default count/log, must-read-0 matrix); counter leaks across cycles (explicit resets + isolation test). S3: relief masking overload (counter names cause).

Non-goals: no admission/schedulability change; no killing; no priority mutation; no kStarvationBreakerTicks change; no SMP.

## 5. SEQUENCING & SPLIT

A (asserts+measurement) → B (mechanism, then conversion) → C (grace+coverage) → D (ledger+relief). Each increment landable alone with its own plan-audit → implement → code-audit chain (file 4 sub-issues #546-A…-D as children, #546 parent stays open; optionally split A→A1/A2, B→B1/B2 at named seams).

Every increment: full debug 3-arch matrix + 3-arch link + check-style 0 + test-history rows before merge; release 2×3 at milestone gate.

## 6. OWNER DECISIONS (before affected increment)

OD-1 watchdog fail-safe ACTION 0 forever vs wire 2 (rec: 0 shipped, 2 reachable). OD-2 grace fixed 500 vs auto 10× measured+floor (rec: auto). OD-3 same-counter limitation accepted vs asymmetric probes (rec: accept+document). OD-4 spin-then-park hook vs exhaustive conversion (rec: hook defaulting to status quo). OD-5 escalation log-only vs force-relief vs kill; EDF preemptible by relief? (rec: force-relief, no killing, no prio mutation). OD-6 confirm no ABI change (rec: none). OD-7 ready-age side table vs TCB field (rec: side table). OD-8 panic level 2 in shipped release builds? (rec: 1 shipped, 2 for certification builds).

## 7. OPEN QUESTIONS (block implementation)

Q1 (OD-4): may SpinLock::lock() call into the scheduler at all, or is sync/-task/ compile-time independence a hard constraint (→ exhaustive conversion, larger diff)? Q2 (OD-1/OD-8): certification posture detect-and-halt or detect-and-degrade? Q3 (OD-3): same-counter acceptable for v0.6.1 or independent probe required? Q4: four child issues with independent audit chains (recommended) or four commits under one audit?
