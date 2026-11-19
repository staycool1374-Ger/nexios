# AUDIT REPORT 2026-11-19T00:00:00Z — issue #583 (design)
PATCH: none (design review of audits/plan-583.md; no implementation diff yet)
FILES: audits/plan-583.md, src/kernel/arch/hal/irq_guard.hpp, src/kernel/task/scheduler_tick.cpp, src/kernel/sync/park_lock.cpp, docs/specs/scheduler.md, docs/specs/irq-preemption-contract.md, docs/specs/test-harness.md

## FINDINGS
- [S3] audits/plan-583.md — kernel.cpp reschedule() call sites not enumerated in-plan (S1 evidence deferred to developer step 14 enumeration `rg -n 'arch::sti\(\)' src/kernel` + reschedule-caller paste)
  WHY: The S1 carve-out safety rests on "no ISR re-enables IF then reaches reschedule()"; the plan correctly obligates the developer to produce the enumeration as implementation evidence, but the design itself cites only the three kernel.cpp sites by assertion — implementation audit must verify the pasted enumeration covers kernel.cpp exception/syscall vectors and that no ISR-path reschedule() exists.
- [S3] audits/plan-583.md §TEST STRATEGY — depth-1 denied leg of resched_park_no_blocked_return pins the old premise only if that leg holds IF==0 at the gate
  WHY: Under the new predicate, nesting!=0 + trap-exempt + IF==1 permits; if the depth-1 leg ever runs IF==1 it flips from denied to parked — implementation must confirm the leg's IF state (IrqGuard-held ⇒ IF==0 ⇒ still denied) and record it in the issue evidence; no test edit is proposed, which is correct, but the premise-preservation claim needs that one measured fact.

## VERIFICATION PERFORMED (spot-checks at tree, base 66e698281 + staged #555 WIP)
1. Missing-conjunct diagnosis CONFIRMED: scheduler_tick.cpp:1305-1307 denies bit 4 on `isr_nesting_own()!=0 && !in_syscall_trap_level() && !waived` (no IF term); park_lock.cpp:162-164 denies IsrContext on `isr_nesting_own()!=0 && !in_syscall_trap_level() && !interrupts_enabled()` — exactly one conjunct differs.
2. Shared-predicate design SOUND: irq_guard.hpp already includes io.hpp + cpu_context.hpp (no new include/dependency); arch-neutral inline, no CONFIG_ARCH_* gate; fail-closed direction preserved (IF==0 always denies); x86_64/aarch64 verdict-unchanged holds (task-context nesting==0 ⇒ predicate false before and after; real-ISR IF==0 ⇒ deny before and after); only rv64 stale-nesting+IF==1 moves, toward the already-audited lock-park verdict.
3. Single-sample claim HOLDS: entry_if sampled once at scheduler_tick.cpp:1477, consumed by bits 4/5 and the park-body restore; only current_task()/waivable reads intervene before the 1497 IrqGuard take — no hlt/switch/blocking point, and an intervening ISR restores IF on return.
4. S1 enumeration PLAUSIBLE with evidence deferred correctly: 56 sti() sites grepped — spot-checked syscall_handlers_irq.cpp:259 (sys_irq_wait trap wait-loop, slot lock dropped) and devfs.cpp:288 (user-task blocked-wait after reschedule) are both task/trap context; no sti() in irq_delivery/interrupt_controller/vectors/isr paths; reschedule() callers are syscall/sync/driver/test paths. Residual ISR-re-enables-IF shape is framed as S1 with a mandatory developer enumeration, and the risk delta is bounded (gate B aligns to the already-audited gate A proof) — correct handling for design stage.
5. rv64 nesting-balance leak correctly EXCLUDED: predicate change does not touch asm counters; g_depth_sat stays the arch-side diagnostic; follow-up ownership framed for the owner. No scope creep into arch asm.
6. Test strategy ADEQUATE: pre-fix rv64 core_sched red-leg capture + post-fix 6 debug runs (core_sync+core_sched × 3 arches) with test-history rows; no test edits (test sanctity); worktree control + 3-attempt breaker; release matrix correctly out of scope (predicate is build-independent).
7. #555 non-intersection HOLDS: staged #555 WIP touches cap/syscall/libc only; plan's touch set is disjoint and step 9 stop-and-report guard covers WIP-induced build breaks.
8. Check-7 artifacts PRESENT: plan lines 3-7 cite graphify query + shortest-path with per-node dispositions (incl. ignored set), obsidian vault searches with EMPTY results recorded, and in-tree line anchors for every normative claim.

DECISION: APPROVED
