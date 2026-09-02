# SIL 3 Audit Report — Issue #2 (IRQ caps + user-space IRQ delivery), Iteration 2 (RE-AUDIT)

- **Auditor:** SIL 3 auditor subagent
- **Date:** 2026-08-29T23:58:15Z (UTC)
- **Branch:** main
- **Input:** `audits/pending_patch.diff` (git diff of issue #2 against main, verified byte-consistent with the working tree for all audited sources)
- **Scope:** Verify iteration-1 findings resolved; detect NEW defects introduced by the corrective patch.

## Summary of iteration-1 corrective claims audited

| Iteration-1 finding | Claimed fix | Verdict |
|---|---|---|
| [S1] sys_irq_register stale reg_idx_ cross-vector hijack | `IrqRegistration::owner` + `slot_belongs_to` before `arm()` | **NOT CLOSED** — see F1 (check-then-act split across two lock acquisitions; `arm()` never revalidates owner/vector under its own lock) |
| [S1] sys_irq_wait post-wake re-lock of reused slot | full revalidation + `r->waiter == t` gated clear in both scopes | **RESOLVED** |
| [S2] isr_entry consumed claimed-but-unarmed slots | `!r->armed \|\| r->vector != vector` gate | **RESOLVED** (fall-through + single EOI verified, see V2) |
| [S2] arm() mutated without slot lock | `arm()` takes `r->lock_` | **RESOLVED** (but see F1) |
| [S3] duplicate `#pragma once`, missing trailing newlines | (claimed fixed) | **NOT FIXED** — duplicate `#pragma once` and missing trailing newlines still present (F5/F6) |

## FINDINGS

### [S1] F1 — `sys_irq_register` ownership check and arming decision are not atomic; `arm()` never revalidates owner/vector → cross-vector hijack still reachable
- **Location:** `src/kernel/syscall/syscall_handlers_irq.cpp:67-75`; `src/kernel/irq_delivery.cpp:131-156`
- **Violated rule:** CODING_STYLE §12.5 (ownership checks are not optional on state-changing ops) and the iteration-1 S1 contract (stale `reg_idx_` must never re-arm a reused slot).
- **Justification:** `slot_belongs_to()` acquires/releases `r->lock_` and then `arm()` re-acquires it, checking only `!r->occupied || r->armed` — never `r->owner`/`r->vector`. A peer task holding a minted (shared, `is_shared()==true`) cap can, in the preemption window between the two critical sections (interrupts are enabled in syscall context), revoke this cap (→ `release_slot_idx`, slot freed) and `create()` a cap for a *different* vector that re-claims the same slot index; `arm()` then arms the *foreign* vector to the caller, hijacking the new cap's delivery and starving its legitimate owner.

### [S2] F2 — `dispose()`/`revoke()` release path: `slot_belongs_to()` then `release_slot_idx()` across two lock acquisitions
- **Location:** `src/kernel/cap/irq.cpp:77-78, 87-88`; `src/kernel/irq_delivery.cpp:202-228`
- **Violated rule:** CODING_STYLE §12.5 (state-changing op must validate ownership in the same critical section).
- **Justification:** `release_slot_idx()` re-checks only `occupied`, not `owner`/`vector`, so a slot drained-and-reused between the outer `slot_belongs_to()` check and the release is spuriously released — the new owner's vector is disarmed, its waiter woken with -1, and the new cap stranded.

### [S2] F3 — `claim_slot()` mutates the shared delivery table without serialization
- **Location:** `src/kernel/irq_delivery.cpp:78-111`
- **Violated rule:** CODING_STYLE §11.6 (concurrently-accessed state must be serialized from every context).
- **Justification:** the free-slot scan and the `occupied/vector` write are unlocked (the corrective patch only locked `set_slot_owner`, after the claim), so two tasks creating caps concurrently can double-claim the same vector and break the single-owner-per-vector invariant.

### [S3] F4 — Dead code: `slot_matches()` declared and defined but never used
- **Location:** `src/kernel/irq_delivery.hpp:110-113`; `src/kernel/irq_delivery.cpp:266-272`
- **Violated rule:** CODING_STYLE §5.3 (defined-but-unused helpers are dead code / an audit finding).
- **Justification:** the corrective patch replaced teardown's ownership guard with `slot_belongs_to()`, leaving `slot_matches()` unreferenced anywhere.

### [S3] F5 — Duplicate `#pragma once` in `irq.hpp` (iteration-1 finding NOT fixed)
- **Location:** `src/kernel/cap/irq.hpp:27,29`
- **Violated rule:** CODING_STYLE §3 ("`#pragma once` only").
- **Justification:** the working tree still contains two consecutive `#pragma once` directives.

### [S3] F6 — Missing trailing newlines (iteration-1 finding NOT fixed)
- **Location:** `src/kernel/cap/irq.hpp`, `src/kernel/test/test_cap_irq.cpp` (EOF byte 0x70/0x7d, not 0x0a)
- **Violated rule:** CODING_STYLE §8 formatting (file must end with a newline).
- **Justification:** both files still lack the final newline in the current tree.

## VERIFIED-CLEAN (iteration-1 fixes confirmed)

- **V1 — sys_irq_wait slot-reuse revalidation (iteration-1 S1/S2 wait paths):** both the pre-wait and post-wake blocks revalidate `occupied && vector==vector && owner==irq && recipient==t` under the slot lock; the waiter entry is cleared only when `r->waiter == t`. A reused slot cannot be consumed on behalf of a foreign vector nor can a foreign waiter be left BLOCKED forever. `r` is a stable pointer into the static `g_irq_regs` array, so re-locking after wake is safe.
- **V2 — isr_entry fall-through / single EOI (iteration-1 S2):** `!r->armed || r->vector != vector` returns false before any EOI; the `handle_interrupt_c` hook (kernel.cpp:1520-1522) returns early only when `isr_entry` returns true, so the tail EOI fires exactly once on both paths. Claimed-but-unarmed vectors fall through to the generic/threaded handler and are never swallowed.
- **V3 — wake discipline:** `isr_entry`/`release_slot_idx` reject TERMINATED/REAPED and verify `generation == waiter_gen`. The `waiter == recipient` invariant (enforced by `sys_irq_wait`'s `recipient != t` check) means `drain_task` never orphans a foreign waiter; every wake path (ISR, release, dispose/revoke) restores the blocked task to READY.
- **V4 — lock ordering / no spinlock-across-reschedule:** all table paths serialize on the per-slot `r->lock_` only; wake/ready ops (`set_task_ready`, `enqueue_ready`, `dequeue_ready`) are IrqGuard + lock-free and never take `scheduler_lock_`, so no `r->lock_ ↔ scheduler_lock_` inversion exists; `sys_irq_wait` releases the slot lock before `dequeue_ready`/`reschedule`.
- **V5 — refcount/owner lifecycle:** single alloc (`MemPool::alloc`+placement new) / release (`dispose` → `MemPool::free`) pair; `owner` is set only after construction and cleared before the block is freed; no window where `owner` aliases freed memory; no double-free.
- **V6 — syscall validation / ENSURE:** all reachable failures return -1 (never ENSURE); `cap::lookup` enforces type + rights + generation; `arm()`/`wait` gate on `reg_idx_ >= 0`; non-x86_64 returns -1 under `#if`.

## Corrective patch

Machine-applicable `git apply`-able patch written to `audits/rejected_patch.diff` (`git apply --check` passes). It fixes F1–F6:

- `arm(reg_idx, cap::IrqCap &owner, TaskControlBlock &recipient)` — owner + vector revalidation inside the same critical section; `sys_irq_register` drops the separate `slot_belongs_to()` check.
- `release_slot_idx(reg_idx, const cap::IrqCap *owner)` — atomic owner revalidation (nullptr = ownerless create-failure path); `dispose()`/`revoke()` drop the outer `slot_belongs_to()` pre-check.
- `claim_slot()` — `arch::IrqGuard` around the find→claim window (uniprocessor discipline, mirrors `drain_zombie_list`).
- Removes dead `slot_belongs_to()`/`slot_matches()` (now unused), the duplicate `#pragma once`, and adds the missing trailing newlines (irq.hpp, test_cap_irq.cpp).

DECISION: REJECTED