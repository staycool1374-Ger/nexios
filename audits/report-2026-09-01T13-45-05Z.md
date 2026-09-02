# AUDIT REPORT 2026-09-01T13-45-05Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/irq_delivery.cpp, src/kernel/irq_delivery.hpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_irq.cpp, src/kernel/test/test_cap_irq_notify.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp

## FINDINGS

**S2 re-verification (issue #7, iter-1 REJECT): FIXED — no residual narrowing path.**
- [OK] `syscall_handlers_irq.cpp:71-75` — raw `arg1 != 0ULL && arg1 != 1ULL` validation now precedes the single `static_cast<IrqDeliveryMode>(arg1)`. 0x101/0x100/2/0xFF all reject with `obj->release()` + `-1` (fail closed, no ref leak). The cast domain {0,1} is within the enum's uint8_t range, so the conversion is well-defined. arm() re-validates under the slot lock (`irq_delivery.cpp:160-162`) as defense-in-depth. Test `irq_notify_mode_validation` now probes 0x101 (`test_cap_irq_notify.cpp:194`), 2, and 0xFF.
- No other cast site for arg1 exists; no residual narrowing path.

**CRITICAL CHECKS (per PROMPT-audit.md):**
1. Dynamic allocations — [OK] No new heap/RT allocations. Delivery stays on the static `g_irq_regs` table; `Notify::notify()` is lock+store only. ISR path is allocation-free.
2. Concurrency boundaries — [OK] `delivery_mode` is read/written only under the per-slot SpinLock: claim_slot (IrqGuard, pre-existing), arm, isr_entry, release_slot_idx, drain_task, and both sys_irq_wait lock scopes. No check-then-act: the mode is immutable for the armed lifetime and both wait scopes re-validate under the lock. New slot→Notify lock nesting is leaf/acyclic (notify() never touches the slot table; ISR-context notify is pre-existing at irq_thread.cpp:117); the wait side releases the Notify lock before reschedule, so no inversion path exists.
3. Assertion masking — [OK] Tests use deterministic handshakes (g_armed/g_inject_done flags; yield_wait_until on observable BLOCKED/armed state). The post-wake `try_wait` re-read handles a documented deschedule-mid-wait window and cannot mask the primary deterministic wake path. No pre-existing assertions changed.
4. Memory safety — [OK] No PMM/BufferPool/double-free changes. Destroyed-Notify ordering verified: `task.cpp:1857 notify.~Notify()` runs before `task.cpp:1870 IrqDelivery::drain_task(*this)`; drain_task skips notify when recipient==&tcb — the only reachable case (recipient is always the arming task; NOTIFY slots can never hold a waiter, so `mine` requires recipient==&tcb). Double-notify is idempotent (value overwrite + single wake; coalescing documented).
5. Critical-section interference — [OK] WAIT backward compat preserved: all existing callers pass arg1=0 (test_cap_irq.cpp:120/320/547), arm defaults to WAIT, claim_slot/reset/release/drain reset to WAIT. sys_irq_wait refuses NOTIFY slots in both lock scopes. release_slot_idx wakes WAIT waiters with -1 (else-if equivalence) and signals NOTIFY Notify with the revoked sentinel 0 — wakers-own-wakeup contract held.
6. Preprocessor semantics — [OK] `(void)arg1` in the non-x86_64 path of sys_irq_register; `irq_notify_ack_pic_state` gated `#if defined(CONFIG_ARCH_X86_64)` at both definition and registration. Mode dispatch is arch-independent; EOI/mask are gated. No asymmetry or uninitialized members introduced.

- [S3] `irq_delivery.cpp:305-315` — the defensive `recipient != &tcb` drain branch retains a check-then-notify that could, in a theoretical concurrent-revoke-during-foreign-cleanup interleaving, call notify() on a Notify destroyed by that foreign task's cleanup. Unreachable in the current syscall-only architecture (documented in the comment), state/gen guards bound it, and worst case is a benign write to destroyed-but-still-allocated memory (TCB freed only after cleanup returns). Same pre-existing check-then-act structure as the WAIT-mode wake path; not a new hazard class.
  WHY: hardening note only — the reachable path (recipient==&tcb) is correctly guarded against the destroyed-Notify ordering trap.

**S3 findings from iter-1 (confirmed still present and documented, non-blocking):**
- [S3] `irq_delivery.cpp:215-219` — ISR-context notify() adds Notify-lock acquisition under the slot lock; leaf/acyclic, same structural pattern as pre-existing ISR slot-lock use. WHY: documented in the code; no lock-order cycle exists.
- [S3] `irq_delivery.cpp:57` / cspace.md §2.6 — revoked sentinel 0 is invisible to Notify::try_wait (notify.cpp:152 requires `notify_value_ != 0`), so polling drivers cannot observe revocation. WHY: documented limitation; blocking drivers wake via wait() with 0.
- [S3] 6/7 new tests un-gated vs arch, mirroring the pre-existing cap_irq convention. WHY: only the PIC-mask test is inherently x86_64.
- [S3] `test_cap_irq_notify.cpp` irq_notify_ack_pic_state arms the test runner's own Notify (stores vector 40, then sentinel 0 on release — self-cleaning). WHY: side effect the WAIT twin lacks; benign and transient.

## DECISION: APPROVED