# AUDIT REPORT 2026-11-19 (code) — issue #583 (rv64 resched_park gate bit 4 IF==1 carve-out)
PATCH: /tmp/pending_patch_583.diff (161 lines, 6 files)
FILES: src/kernel/arch/hal/irq_guard.hpp, src/kernel/task/scheduler_tick.cpp, src/kernel/sync/park_lock.cpp, docs/specs/scheduler.md, docs/specs/irq-preemption-contract.md, docs/specs/test-harness.md

## FINDINGS
- [S3] test-history.txt 09:17–09:20 window — 8 rows for 6 gates (1 TIMEOUT + 1 FAILED:1 + 6 green; retries green)
  WHY: Pre-declared rv64 TCG flake sensitivity (plan S2 mitigation) covers this shape and the final state is 6/6 green, so it is evidence noise, not a defect — recorded so the flake rate stays visible.

## VERIFICATION PERFORMED (static, tree @ worktree with #555 F2 WIP present but untouched)
1. Shared predicate defined once: `arch::isr_context_park_denied(bool)` at irq_guard.hpp:92, directly below `in_syscall_trap_level()` (:69); no new #include (io.hpp + cpu_context.hpp already at :25-26), no new storage (pure inline read of `isr_nesting_own()` + trap level + param). Doc block carries all three conjuncts, per-arch IF-cleared proof, stale-count rationale, fail-closed direction, and the exact two-caller contract.
2. Per-arch IF proof confirmed in-tree: x86_64 isr_stubs.asm:164 `cli`; aarch64 vectors.S:112/241 `daifset`; riscv64 syscall_entry.S:62/552 `csrci sstatus, 2`.
3. Both gates call it: scheduler_tick.cpp:1314 passes pre-guard `entry_if` (sampled once at :1477 per plan; no re-read inside helper by construction — param is `bool`); park_lock.cpp:157 passes `arch::interrupts_enabled()`. Park-lock expression provably identical to before (conjunct-for-conjunct: nesting!=0, !trap-level, !IF). Resched side is the intended single-conjunct delta (+IF term). No inline duplicate predicate anywhere (grep: 2 call sites + 1 definition).
4. `&& !waived` retained verbatim at :1314 with the redundant-comment (:1309-1313); bits 2/3/5 lines untouched, bits 9-13 outside the single hunk — untouched.
5. Docs parity exact: scheduler.md INV-4a names the predicate + stale-baseline note; contract §4 restated as the implemented predicate + "divergence is a defect (#583)"; test-harness.md OQ1 rewritten to closed state with redundant-`!waived` retention noted.
6. Design-verdict S3 watch-item 1 (S1 enumeration) CLOSED: 56 `arch::sti()` sites all in syscall/IPC wait-loops, park bodies, guard/IrqGuard restore, task entry, or tests — none in irq_delivery/interrupt_controller/vectors/ISR paths; kernel.cpp reschedule() sites (2637/2690/2728) are exception/syscall/signal vectors = trap context; virtio_blk.cpp:680 + ahci.cpp:713 explicitly comment "No reschedule() from ISR". No ISR-path sti, no ISR-path reschedule.
7. Design-verdict S3 watch-item 2 (denied-leg premise) CLOSED: depth-1 leg of `resched_park_no_blocked_return` runs under the suite outer IrqGuard ⇒ IF==0 at the gate ⇒ predicate still denies (given stale nesting!=0); bit 2 denies independently anyway. Denied leg cannot flip to parked.
8. rv64 nesting-balance leak untouched: patch touches no .S/.asm/counter code. No test files in patch (file list above only). #555 F2 tree dirt (cap/syscall/libc) disjoint from patch — no interaction observed.
9. Evidence: test-history.txt rows 7556-7562 show 6 green core_sched/core_sync runs (127/103-class counts, 0 FAILED); style-errors-0 claim accepted on inspection (added code matches file style; checker run is developer-side).

## LIMITATIONS
- Issue #583 thread (graphify/vault paste + S1 enumeration paste + gate logs) not reachable with my toolset; check-7 plan-stage artifacts were verified PRESENT by design-review-583-r1. Test-history rows verified in-tree as substitute evidence.

DECISION: APPROVED
