# AUDIT REPORT 2026-09-01T07-20-38Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/irq_delivery.cpp, src/kernel/irq_delivery.hpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_irq.cpp, src/kernel/test/test_cap_irq_notify.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S2] src/kernel/syscall/syscall_handlers_irq.cpp:68 — fail-closed mode validation defeated by uint8_t truncation
  WHY: `auto mode = static_cast<IrqDeliveryMode>(arg1)` narrows a user-controlled uint64_t to the enum's uint8_t underlying type BEFORE arm()'s validation, so any out-of-range arg1 whose low byte wraps to 0 or 1 (e.g. 0x101 -> NOTIFY, 0x100 -> WAIT) is silently accepted — directly violating the patch's own documented "unknown modes fail closed" contract (irq_delivery.cpp comment + cspace.md §2.6); the new test only probes 2 and 0xFF, missing the wrapping class.

- [S3] src/kernel/irq_delivery.cpp:223 — ISR-context notify() acquires the recipient's Notify lock_ inside the slot lock
  WHY: isr_entry now takes slot lock -> Notify lock from ISR context; if the ISR preempts the recipient while it holds its own Notify lock_ (bounded critical sections in Notify::wait/notify/try_wait, e.g. inside the pre-existing sys_notify syscall), the ISR spins forever — a hard hang. Lock order is leaf and acyclic (notify paths never touch the slot table) and the same structural ISR-side-spinlock pattern already exists for the slot lock in the WAIT path, so this is a hardening note, not a new blocker.

- [S3] src/kernel/irq_delivery.cpp:57,271,313 — revoked sentinel 0 is invisible to Notify::try_wait()
  WHY: sync::Notify::try_wait (notify.cpp:152) requires `notify_value_ != 0`, so a NOTIFY-mode driver polling try_wait can never observe revocation; only blocked sys_notify_wait drivers (the documented contract) are woken by notify(0). Note for spec/users, not a safety defect.

- [S3] src/kernel/test/test_cap_irq_notify.cpp:31-569 — 6 of 7 tests are NOT #if CONFIG_ARCH_X86_64-gated yet assert sys_irq_register success, which returns -1 on aarch64/riscv64
  WHY: on non-x86_64 test builds the class registers only 6 cases (mismatching the 7-count in test_expected_counts.hpp) and the core tests would fail; this exactly mirrors the pre-existing cap_irq pattern (11+1) and the suite's counts are x86_64-oriented, so consistent with convention but worth noting.

- [S3] src/kernel/test/test_cap_irq_notify.cpp:538-544 — arming the test runner itself in NOTIFY mode stores vector 40 on the runner's own Notify
  WHY: isr_entry delivers to `*Scheduler::current_task()` and release resets the value to 0, so harmless in isolation; a side effect the WAIT-mode twin test does not have.

Checksum of the six critical checks: (1) no dynamic allocation on isr_entry/release/drain notify paths; (2) delivery_mode read/written only under the per-slot SpinLockGuard (arm/claim/isr_entry/release_slot_idx/drain_task/sys_irq_wait both scopes) — no check-then-act TOCTOU, mode immutable for arm lifetime; (3) test fixture spin on state==BLOCKED + try_wait re-read does NOT mask a Heisenbug — Notify::notify stores the value before waking and wait() reads it after, and a lost wakeup would manifest as a timeout (fail), not a false pass; (4) no UAF — drain_task never notifies the dying task's destroyed Notify (cleanup destroys ~Notify at task.cpp:1857 BEFORE drain_task at 1870, guard `r.recipient != &tcb`), all notify() calls carry TERMINATED/REAPED+generation guards, and dispose/revoke set reg_idx_=-1 making a release-after-drain idempotent (no double notify); (5) slot->Notify nesting is leaf/acyclic and WAIT-mode backward compatibility holds (arm default WAIT, claim_slot initializes WAIT, sys_irq_wait requires WAIT — pre-existing cap_irq tests unaffected); (6) no asymmetric #ifdef semantics in kernel code (NOTIFY delivery is unreachable on non-x86_64 exactly like WAIT, matching kernel.cpp:1585 gating).

## PATCH
REJECTED — `audits/rejected_patch.diff` was written: it validates the raw 64-bit arg1 (0/1 only) in sys_irq_register before the narrowing cast so out-of-range modes fail closed, and extends irq_notify_mode_validation with a wrapping-value probe (0x101) to lock the fix in.

DECISION: REJECTED