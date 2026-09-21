# SIL 3 Audit Report — Issue #2 (IRQ caps + user-space IRQ delivery), Iteration 3 (RE-AUDIT)

- **Auditor:** SIL 3 auditor subagent (read-only; writes to `audits/` and `/tmp` only)
- **Date:** 2026-08-30T00:08:05Z (UTC)
- **Branch:** main (verified `git branch --show-current`)
- **Input:** `audits/pending_patch.diff` (git diff of issue #2 against main; verified byte-consistent with the working tree for all audited sources)
- **Scope:** Confirm iteration-2 findings F1–F6 closed; detect NEW defects from the corrective patch; confirm no regression of verified-clean iteration-2 items; re-verify concurrency invariants.
- **Rules:** `prompts/CODING_STYLE.md` §5.1 (ENSURE), §11 (concurrency), §12 (lifecycles); scope items 1–5.
- **Reference patterns:** `Semaphore::wait()` (blocking wait), `Notify::notify/wait` (wake discipline), `KernelObject` (refcount), `Scheduler::reschedule/set_task_ready` (deferred switch).
- **Empirical gate evidence (test-history.txt):** `cap_irq PASSED: 12 FAILED: 0 TIME: 1910ms` (2026-08-30 14:05, corrective patch applied); `all PASSED: 963 FAILED: 0` (14:20, debug gate, trace OFF); `release_all PASSED: 85 FAILED: 0` (14:25).

## FINDINGS

No S1/S2 findings.

### Iteration-2 findings F1–F6 — verification of closure

- **F1 (S1) — `sys_irq_register` slot_belongs_to→arm TOCTOU: CLOSED.** `IrqDelivery::arm()` (`src/kernel/irq_delivery.cpp:140-143`) now performs the ownership + vector revalidation (`!r->occupied || r->armed || r->owner != &owner || r->vector != owner.vector`) and the arming decision inside ONE critical section under `r->lock_`. `sys_irq_register` (`syscall_handlers_irq.cpp:70`) calls `arm()` directly with no separate pre-check; any concurrent revoke/dispose/drain + slot reuse between the `reg_idx_` read and the lock is caught by the under-lock revalidation (fail closed). CODING_STYLE §12.5 satisfied.
- **F2 (S2) — `dispose()`/`revoke()` split-lock release: CLOSED.** `release_slot_idx(reg_idx, owner)` (`irq_delivery.cpp:205-217`) performs the ownership check and the release in one atomic critical section (`!r->occupied || (owner != nullptr && r->owner != owner)` → fail). `IrqCap::dispose()`/`revoke()` (`cap/irq.cpp:79-80, 89-90`) call it directly with `this`, no outer pre-check. A drained-and-reused slot is never spuriously released. `owner == nullptr` is reachable only from `create()`'s alloc-failure path (`irq.cpp:56`), which targets the idx it just claimed (ownerless, occupied) — no wrong-target release is possible.
- **F3 (S2) — `claim_slot()` unlocked mutation: CLOSED.** `claim_slot()` (`irq_delivery.cpp:80-118`) is wrapped in `arch::IrqGuard` (find→claim window un-interleavable on the uniprocessor, `drain_zombie_list` discipline). During the window the slot is inert (ownerless claim; `arm()`/`release_slot_idx`/`isr_entry` all fail-closed on a null/absent owner, unarmed slot), so no cross-vector double-claim window exists. `set_slot_owner` (`irq_delivery.cpp:120-128`) binds ownership under `r->lock_`.
- **F4 (S3) — dead `slot_matches()`/`slot_belongs_to()`: CLOSED.** Both removed (no definition or call remains; only comments reference the old names). All table paths go through `find`/`slot`/`arm`/`release_slot_idx`/`drain_task`/`set_slot_owner` — all referenced.
- **F5 (S3) — duplicate `#pragma once`: CLOSED.** `src/kernel/cap/irq.hpp` carries exactly one.
- **F6 (S3) — trailing newlines: CLOSED.** All six new/modified sources end with byte `0x0a` (verified for irq.hpp, irq.cpp, irq_delivery.hpp, irq_delivery.cpp, syscall_handlers_irq.cpp, test_cap_irq.cpp).

### Scope item 2 — NEW defects from the corrective patch

- **Include cycle:** none. `irq_delivery.cpp` now `#include <kernel/cap/irq.hpp>` (needed for the full `IrqCap` type used by `arm()`'s `owner.vector` access). `irq.hpp` includes only `types.hpp` + `kernel_object.hpp` (which includes only `types.hpp`); `irq_delivery.hpp` forward-declares `IrqCap`. No cycle.
- **`arm()` signature change:** sole callers are `sys_irq_register` (`syscall_handlers_irq.cpp:70`) and the test `irq_delivery_ack_pic_state` (`test_cap_irq.cpp:630`) — both updated to the `(reg_idx, IrqCap&, TaskControlBlock&)` form.
- **`release_slot_idx` return-value handling:** `dispose()`/`revoke()` intentionally ignore the bool (a `false` return means the slot is no longer owned by this cap — the correct outcome for a drained/reused slot). No unhandled-error path.
- **Build integration:** the Makefile globs `src/**/*.cpp` (`mk/rules.mk:15`), so `syscall_handlers_irq.cpp`, `cap/irq.cpp`, `irq_delivery.cpp`, `test_cap_irq.cpp` are compiled automatically; non-x86_64 builds compile the syscall stubs (`#if CONFIG_ARCH_X86_64` bodies return -1). The `syscall_table_` array has 59 entries (index 57 = `sys_irq_register`, 58 = `sys_irq_wait`), matching `MAX_SYSCALL = 59` and the `IRQ_REGISTER = 57 / IRQ_WAIT = 58` enum values. Empirical gates confirm the build is green.
- **Placement new** in `irq.cpp` follows the established per-TU `inline` pattern already used by cap.cpp/frame.cpp/mmio.cpp/untyped.cpp — not a new construct.

### Scope item 3 — no regression of verified-clean iteration-2 items

- **V1 wait-path revalidation:** `sys_irq_wait` revalidates `occupied && vector == vector && owner == irq && recipient == t` under the slot lock in both the pre-wait block (`syscall_handlers_irq.cpp:124-128`) and the post-wake block (`:177-178`); the waiter entry is cleared only when `r->waiter == t` (`:183-186`). A reused slot can neither be consumed for a foreign vector nor orphan a foreign waiter.
- **V2 isr_entry fall-through + single EOI:** `!r->armed || r->vector != vector` (`irq_delivery.cpp:176`) returns false before any EOI; the `kernel.cpp:1520-1522` hook returns early only on `true`, so the tail EOI fires exactly once on both paths. The EOI calls (`APIC::eoi()` + PIC EOI for 32–47) match the tail path exactly.
- **V3 wake discipline:** `isr_entry` (`irq_delivery.cpp:184-190`) and `release_slot_idx` (`:229-233`) reject `TERMINATED`/`REAPED` and verify `generation == waiter_gen` before `set_task_ready` (Notify discipline). The `recipient == waiter` invariant (enforced by `sys_irq_wait`'s `recipient != t` rejection at `:104`) guarantees `drain_task` can never orphan a foreign waiter.
- **V4 lock ordering / no spinlock across reschedule:** `sys_irq_wait` releases `r->lock_` before `dequeue_ready`/`reschedule` (`:143-144`); the interrupts-disabled rollback re-locks only to clear its own waiter entry. Lock order is uniformly slot-lock → scheduler/ready-queue (isr_entry, release_slot_idx, arm, drain_task), never the reverse — no inversion. `set_task_ready` takes its own IrqGuard; the ISR path can spin only briefly on a slot lock never held across a reschedule.
- **V5 refcount/owner lifecycle:** single alloc (`MemPool::alloc` + placement new) / release (`dispose` → `MemPool::free`) pair; `r->owner` is set after construction (`set_slot_owner`) and cleared by release/drain before the block is freed; the syscall paths hold a lookup ref, so no owner/waiter can dangle.
- **V6 syscall validation / ENSURE:** all reachable failures (bad type/rights/gen, revoked cap, reg_idx_ < 0, already armed, not-armed-for-task, interrupts-off rollback, non-x86_64) return -1 or nullptr — never ENSURE (grep confirms ENSURE appears only in comments).

### Scope item 4 — S3 hygiene

- No dead code introduced (all table functions referenced); single `#pragma once`; trailing newlines present; no unbounded loops in kernel paths (`claim_slot`'s scan is bounded by `CONFIG_CAP_MAX_IRQ`; `find`/`drain_task` bounded).

### Scope item 5 — concurrency

- **BLOCKED dequeued (INV-2):** `sys_irq_wait` transitions to `BLOCKED` under the lock, then `dequeue_ready(*t)` before `reschedule()` (`:143`) — a blocked task is never physically queued.
- **No lock held across reschedule:** the waiter registration lock scope closes at `:141`; only the interrupts-off rollback re-takes the lock (with IRQs off, no ISR can contend).
- **Generation-guarded wake:** waiter/recipient pointers carry `waiter_gen`/`recipient_gen` set at registration/arm time; both wake paths and the drain verify state + generation (Notify discipline).
- **Never BLOCKED forever:** every wake path is covered — ISR (`isr_entry`, armed delivery), `release_slot_idx` (dispose/revoke wakes with -1), `drain_task` (only when the dying task IS the waiter/recipient, which is by construction the same task, so no live task is orphaned). All three null the waiter under the lock so the wake fires exactly once.

## DECISION: APPROVED

- Iteration-2 F1 (S1), F2/F3 (S2), F4–F6 (S3) are all closed: ownership+arming, ownership+release, and find+claim are now single atomic critical sections; dead code, duplicate pragma and newline issues are gone.
- No new S1/S2/S3 defects found in the corrective patch (include graph, arm/release callers, return handling, syscall-table sizing all verified).
- Concurrency invariants (INV-2 dequeue, no spinlock across reschedule, generation-guarded single wake, no permanently-blocked waiter) hold; empirical gates pass (`cap_irq` 12/12, `all` 963/0 debug, 85/85 release).