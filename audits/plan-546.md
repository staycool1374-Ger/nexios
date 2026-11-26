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

## 1. INCREMENT A — IRQ contract: stated → enforced (no new locks, no new ABI) — REVISION R2

Closes audits/report-20261126-546A-plan.md ([F1]–[F7] markers). Retrieval verified (graphify node + obsidian/docs reads; tree facts confirmed).

### A.1 New module and surface

New `src/kernel/sync/irq_contract.{hpp,cpp}`: `ExecCtx::classify()`, check entry points (`check_wait_ctx`, `check_block_store`, `check_park_entry`, `syscall_exit_epilogue`, `flush_halt`), violation counters per class, IRQ-off lateness high-water, deferred halt-request latch, test-isolation reset. `irq_guard.hpp` outer-transition dwell timestamps. `on_tick` entry lateness sample. INV-C1 de-gated from `CONFIG_DEBUG` under policy `CONFIG_IRQ_CONTRACT_ASSERT` (0/1/2, identical semantics both builds). `Scheduler::note_block_transition(row, actor)` after each of the 40 BLOCKED stores (table verified exact, not re-derived). `syscall.cpp` real exit assertion + six checkpoint renames. `park_lock.cpp` park-entry context assertion + escalation bookkeeping (no bound). `kernel::sync::DeclaredWaitScope` marker (internal-only, no ABI, no lock). Docs: contract §1/§3/§8 + new §9 failure policy + ledger assert-coverage column.

### A.2 Normative invariants (new IDs, defined here)

INV-CTX-1 (wait-context legality): BLOCKED store, park arm, or sti-owning wait only in wait-capable contexts {Task, SyscallTrap} as classified from `isr_nesting_own()`, `in_syscall_trap_level()`, `irq_guard_depth_own()/owner`; Isr/Boot ⇒ class C1. Live self-owned IrqGuard depth is recorded, never asserted.
INV-CTX-2 (wait IRQ ownership): sti-owning wait enters with depth==0 or inside a `DeclaredWaitScope` naming a closed-list ledger row; restores the exact (depth, owner, entry_if) triple; re-enable with depth!=0 and no scope ⇒ C2.
INV-BLK-1 (one post-store note per transition): every BLOCKED store followed in the same non-preemptible region by exactly one `note_block_transition(row, actor)`; duplicates and misses ⇒ C4 (ledger-audit/QE, no runtime assert).
INV-BLK-2 (actor attribution): actor computed once from the target pointer — SELF if `target == current`, else FOREIGN (incl. scheduler_suspend.cpp:179, scheduler_debug.cpp:87, scheduler_watchdog.cpp:192, sys_kill-adjacent paths). INV-CTX-1/2 evaluate the ACTOR; INV-BLK-1's duty evaluates the TARGET. Counters attribute to target->id (or 0); actor ExecCtx recorded in the same row. [F1]

### A.3 DeclaredWaitScope — bounded exemption [F2]

RAII marker (`kernel::sync`, internal, non-copyable/non-movable): ctor takes a non-null literal matching a closed-list ledger row, stores entry triple in one per-CPU slot, saturating depth 0..1 (nesting forbidden — second construction is C3, keeps outermost row); dtor verifies exit triple (mismatch ⇒ C3); must not outlive its function, span a syscall return, or span another scope. Suppresses ONLY C2 for its own frame. Closed list (fixed at approval): W-1 park_lock.cpp:117-123, W-2 syscall_handlers_ipc.cpp:86-99 (+409-416, 445-463 variants), W-3 syscall_handlers_thread.cpp:590-596, W-4 scheduler_tick.cpp:1485-1495. New rows need plan amendment + auditor sign-off. Ledger records every use with measured triple; QE `declared_wait_ledger_complete` asserts tree count == ledger rows.

### A.4 Violation taxonomy + degraded/halt boundary [F3 — closes Q2/OD-8 for A]

New contract §9 + microkernel-transition §9 cross-ref + §12 config row. Classes: C1 (Isr/Boot wait), C2 (undeclared depth!=0 sti wait), C3 (scope misuse), C4 (missed/duplicate note — ledger/QE only), C5 (INV-C1 SCHED+15), C6 (SelfLock SCHED+16 — never panics, exempt). Policy: 0 = compiled out; 1 = default shipped both builds (classify + count + one dmesg WARN per class per boot, always continue, no panic); 2 = certification builds only (count + deferred halt via atomic halt-request latch, flushed at next safe point — syscall exit epilogue or reschedule entry with IF==1; one fire, never re-entered; C6 exempt in both profiles).

### A.5 INV-C1 de-gate gate [F4]

De-gate commit (default stays 1) then separate default-flip commit only when green. Matrix: 3 arches × 2 builds × classes synchronization_irq_guard, spinlock, lock_order, scheduler_core, core_sync, core_sched, core_syscall, ipc, ipc_core, ipc_blocking, drivers_core, selftest (+ arch_aarch64/riscv64 on-arch). Debug full registry, release TF_RELEASE subset; SCHED+15 family zero everywhere; C1/C2/C3 zero; C6 ≤ baseline. SCHED+16 stays CONFIG_DEBUG-gated WARN-only, never panics. Cost: +1 per-CPU load + compare per reschedule (measured via §8 method). Soak: 72h x86_64 debug+release paired, 0 C1/C2/C3/C5, no watchdog FIRE, no tracker delta, C6 ≤ baseline. Owner: QE runs matrix + soak + rows; dev owns flip decision. Rollback: any non-harness C5 hit or release reschedule p99 lateness regression > 5% ⇒ revert de-gate commit, file site issue.

### A.6 Measurement cost, honestly [F5]

Every IrqGuard outer transition + every on_tick entry, per build: release = one raw_counter read + saturating compare + conditional store (no shift/mask/log/ring); debug = same triple + (counter & 63)==0 debug-only sample line. "1 counter read" is the always-on cost in both builds; "factor 64" is debug-only logging, not the high-water mechanism. High-water updates on EVERY transition (true maximum, not sampled); 1-in-64 feeds only the evidence line. A sampled maximum is non-normative by rule (never a WCET input).

### A.7 Syscall exit placement [F6]

Check logic in one place — `irq_contract::syscall_exit_epilogue(tag)`, inlined, invoked at all 6 return paths of handle/handle_fast (4 + 2: bounds-fail, fast/full dispatch, canary-fail). Rename mapping: 140→syscall_entry:handle:bounds, 168→handle:fast_pre_dispatch, 180→handle:canary_fail, 184→handle:full_pre_dispatch, 191→handle_fast:bounds, 201→handle_fast:pre_dispatch; new exit tags handle:fast/full_post_dispatch, handle_fast:post_dispatch, bounds_fail, canary_fail. Epilogue asserts SyscallTrap ctx, depth 0, spinlock clean, IF 0; effects atomics-only + interrupts_enabled read + retained checkpoint.

### A.8 Entry-point constraint [F7]

note_block_transition + all irq_contract entries + DeclaredWaitScope ctor/dtor + park-entry assert: lock-free, allocation-free, no scheduler services, atomics only (precedent: tick_watchdog::checkpoint, schedoracles counters). Prohibited: any SpinLock/ParkLock take, scheduler_lock_ take, PMM/MemPool/new/delete, direct serial write, reschedule()/scheduler services. Evidence deferred to flush_halt/rate-limited latch. Safe at any ISR nesting by construction.

### A.9–A.16 (unchanged from R1)

40-site ledger with actor/assert-coverage/ctx-reachability columns (exact at audit time); shared-arch seams + x86 TSC gate + aa64/rv64 duration caveat; no new sampling key (CONFIG_DEBUG discriminator; CONTRACT_ASSERT 0/1/2 documented); docs deliverables (transition §1 rows, contract §1/§3/§8/§9, ledger columns + scope rows); QE stubs incl. violation_class_matrix, halt_latch_single_fire, declared_wait_ledger_complete; gates incl. A.5 3×2 matrix; risks S1/S2/S3 as R1; non-goals unchanged (no block rules, no probe, no conversions, no ratchet, no cert target, no SMP).

## 2. INCREMENT B — Spinlock discipline: one story, enforced, complete (no new locks on kill/IRQ paths) — REVISION R2

Closes audits/report-20261126-546B-plan.md [F1]–[F8]. Retained unchanged: weak-seam direction, A-dependency (INV-BLK-1 landed `44bba28d6`; `park_window` remains the single BLOCKED store/note for parks, `park_lock.cpp:113`), cross-arch seams, no C/D creep, non-goals. B1 = mechanism, B2 = conversion ([F5]).

### B.1 Mechanism — bound, checkpoint, hook seam [F2][F5]

`SpinLock::lock()` (spinlock.hpp:53) becomes: first exchange fails ⇒ bounded pause loop of `CONFIG_LOCK_CONTEND_ROUNDS` rounds with `tick_watchdog::checkpoint("lock_contend")` every 256 rounds (`(round & 255U) == 255U`) ⇒ exactly one `sync::spin_contended_hook(*this)` ⇒ `Acquired` returns immediately (hook has fully published the take); `Retry` re-enters the bounded loop. `try_lock()` (spinlock.hpp:83) never calls the hook. New `void lock_irq_atomic() noexcept` = the same bounded/checkpointed loop without the hook; its only callers are `IrqSpinLockGuard` ctor and re-lock (irq_spinlock_guard.hpp:22,45 — grep gate: exactly 2 call sites, both in that header).

Hook contract (exact). New `src/kernel/sync/spinlock_hook.hpp` (includes `<types.hpp>` only): `enum class SpinContendAction : uint8_t { Retry = 0, Acquired = 1 };` `SpinContendAction spin_contended_hook(SpinLock &lock) noexcept;` Weak default in new TU `src/kernel/sync/spinlock_hook.cpp` (`__attribute__((weak))`, returns Retry, no calls, no diagnostics). Strong override in `src/kernel/sync/park_lock.cpp` (owns the single park predicate; reuse park_window/take_success/note_denied/note_exhaustion verbatim, no new BLOCKED store, no new note_block_transition row). Q1 no-include rule: include closure of spinlock.hpp = {arch/io.hpp, lib/atomic.hpp, sync/spinlock_order.hpp, sync/spinlock_hook.hpp, task/tick_watchdog.hpp (types-only)} and must NOT contain task/scheduler.hpp, task/task.hpp, sync/park_lock.hpp — mechanical grep gate. A.8-equivalent entry constraint: fast/bound path + hook are lock-free w.r.t. other locks, allocation-free, no scheduler service, ISR-callable; no serial outside latch-gated lines; no panic. No-park targets: rank Sched ⇒ note_denied(SchedLock) + Retry. New config keys `CONFIG_LOCK_CONTEND_ROUNDS` (1024, 64..65536) and `CONFIG_LOCK_HOLDER_PROBE` (0/1, default 1) with #error ranges. Latch capacity kSiteLatchSlots 16 → 64.

### B.2 INV-LOCK-1..4 — normative [F1]

INV-LOCK-1 (bounded acquisition): every failing first exchange runs ≤ CONFIG_LOCK_CONTEND_ROUNDS pause rounds with checkpoint every 256 before exactly one hook decision; structural + counted, never panics. INV-LOCK-2 (park only when allowed): hook parks only if lock_park_allowed() and rank != Sched; else Retry with counters/latch lines only. Deny + count. INV-LOCK-3 (no park in non-wait-capable/lock-holding contexts): 12 ParkDenyReason arms via single lock_park_allowed() consumed by both gates (divergence = defect); entry IF==0 is not a deny; IrqSpinLockGuard never reaches the hook. INV-LOCK-4 (rank completeness): (a) every production sync::SpinLock has exactly one ledger row; (b) ranked nesting strictly increasing Sched(1)→Queue(2)→Leaf(3); (c) no hook park targets Sched. (a)/(c) QE static gate rank_table_complete; (b) report-only violation count + one-shot WARN, M8 owns enforcement.

### B.3 Rank table completeness + ledger authority [F3]

No new ranks. Ranked: scheduler_lock_ Sched (task/scheduler.cpp:189); sync::Queue::lock_ Queue (sync/queue.hpp:51); MessageQueue::lock_ Queue (ipc/ipc.cpp:101); DeathNotify::s_lock_ Leaf; g_iopb_lock Leaf; g_derivation_lock Leaf; g_hold_lock Leaf. Unranked-with-reason (closed tokens ISR-reachable / allocator-IF0 / sched-nested / M8-deferred): urc s_lock, g_stop_lock, g_bp_lock, iommu g_lock_, brk_lock, g_bind_lock, mempool_lock_, pmm_lock_, ElfLoader::lock_, PagerRegistry::s_lock_, ForeignFrameMap::s_lock_, cap (CapabilitySpace/VSpaceMap/FrameMap/CarveBinding/MmioWindow/Endpoint), TaskControlBlock::lock_+cwd_lock_, zombie_lock_, TimerWheel::lock_, PosixTime::lock_, IrqDelivery::lock_, DmaChannel::lock_ (×2), VirtioNetDev::lock_, ElfShared::lock_. New docs/lock-rank-ledger.md authoritative (ledger row count == definition count, rank equality, exactly one Sched, closed tokens + m8_status); contract §2 becomes a matching summary. M8 handoff: row leaves Unranked only with M8 nesting evidence; no row fails B1's gate.

### B.4 Holder probe on the new bound-exceeded path [F6]

Placement: only the new lock() bound-exceeded path inside the strong hook's deny branch (not ParkLockGuard re-arm / note_exhaustion). Config CONFIG_LOCK_HOLDER_PROBE (0/1, default 1); 0 compiles out; one-shot per (lock, site) latch. Allowed accessors (read-only, existing): spinlock_dump_held / spinlock_find_holder / spinlock_lock_last / lock.is_locked_for_debug()/holder_for_debug() / find_task snapshot / current_task; output shape = note_exhaustion line family. Prohibited: any lock take, allocation, reschedule/scheduler service, serial outside latch-gated Logger::error, any state change besides latch + one counter.

### B.5 Exact conversion set (B2) [F4]

R-CONV rule: convert iff first lock_ acquisition lexically reached in a public function whose body contains Scheduler::reschedule() or whose name is remove_waiter; post-wake re-takes untouched. 16 sites: queue.cpp remove_waiter:190, send:206, send_err:264, receive:356, receive_err:415; notify.cpp wait:110, wait_err:177; eventgroup.cpp remove_waiter:112, wait_bits:165, wait_bits_err:212; semaphore.cpp remove_waiter:76, wait:206, wait_err:258; mutex.cpp remove_waiter:106, lock:247, lock_err:353. Rewrite: ParkLockGuard guard(lock_, "obj:fn"), early unlock→guard.release(). 38 inner re-takes stay SpinLockGuard (post-wake transfer, rollback fail-fast, wake-side ISR-reachable, try/diagnostics/unlock). IrqSpinLockGuard sites keep type → lock_irq_atomic() (pmm/mempool/compl/dma/virtio-net/shootdown/urc/iopb — allocator/IF0 recursion safety). Safety case: inner_no_park pairs with LockHeld deny (park_lock.cpp:189-190); wrongly converted site denied + spins, asserted by stub (park delta 0, reason LockHeld, hook calls ≥ 1, contender stays RUNNING).

### B.6 Cross-arch seams + new caller class [F7]

arch::pause() all three; shared-C++ hook + lock_irq_atomic, no arch ifdefs. Per-arch INV-L1' contended-lock cases (IF=1 wedge→park→acquire+IF restore; IF=0 under guard→deny+IF preserved; IF=0 no-guard trap shape→park allowed+restored). QE thresholds: bounded_rounds (hook calls 1..2, ≤2×CONTEND_ROUNDS), park_from_primitive (+1 park, acquire, IF restored, no exhaust), inner_no_park (delta 0, LockHeld, RUNNING), holder_blocked_detected (one probe line, no repeat, 0-build silent), rank_table_complete, rank_order_sched_first (Sched→Queue +0, Queue→Sched +1 no panic), reset_isolation, site_latch_capacity (<64).

### B.7 B1/B2 split, gates, cost, soak, rollback [F5][F8]

B1 (mechanism, zero conversion): bound + hook seam + lock_irq_atomic + 2 config keys + 64 slots + rank ledger/table + probe + INV-LOCK + docs + stubs. B1-green gate: synchronization_lock_order/sync/locking, core_sync, scheduler_core, core_sched, ipc, ipc_core, ipc_blocking, drivers_core, selftest × {x86_64, aarch64, riscv64} debug + arch_aarch64/arch_riscv64/hal_core/hal_timer; criteria 0 FAILED, overflow/unpaired/fire/exhaust/ResourceTracker all 0, fallback ≤ baseline, x86_64 release core_sync+selftest green, 3-arch link, check-style 0. B2 (conversion only): 16 sites + labels + release() rewrite. B2-green: B1 matrix + core_syscall + ipc_blocking, five sync classes ×3; park_count>0 with 0 exhaust, C1..C6 ≤ baseline, site_latch_capacity pass. Cost bar: uncontended ≤ +2% p50 / bench_syscall p99; contended ≤ +5% p99 over 10^6 takes (raw_counter deltas; sampled maxima non-normative). Soak 72h x86_64 debug+release paired + 2h aa64/rv64; 0 FIRE, 0 new exhausted sites, tracker 0, selftest ×5 green. Two-commit split + revert triggers per commit (B1: non-harness FIRE / >5% p99 / park-where-denied; B2: C-class over baseline / sync hang / inner_no_park fail).

### B.8 Risks

S1: park where non-blocking assumed or bare-cli atomic (mitigate: hook no-op unless allowed, IrqSpinLockGuard→lock_irq_atomic, LockHeld/Sched denies, stubs, per-arch IF cases, two-commit revert). S1: wrong/missing rank narrowing INV-LOCK-4 (mitigate: ledger + rank_table_complete gate). S2: weak/strong mis-link (mitigate: park_from_primitive fails without park + nm gate). S2: latch slots overflow (mitigate: 64 + capacity stub). S2: probe cost/lock-take (mitigate: allowlist, read-only, one-shot latch). S3: park-count re-baselines; cost noise ±2%.

### Non-goals

No PI; no ISR/device conversion; no new ranks; inversions report-only; no ipc.cpp conversion (M2 landed); no SMP; no C/D creep; no change to A rows (park_window:113 stays the only park note row).

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
