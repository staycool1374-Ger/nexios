# SIL 3 Audit — issue #2 (IRQ caps + user-space IRQ delivery)

- **Date:** 2026-08-29T23:50:42Z
- **Auditor:** SIL 3 subagent (read-only; outputs to `audits/` and `/tmp` only)
- **Scope:** `audits/pending_patch.diff` (issue #2 against `main`) — IrqCap,
  `IrqDelivery` table, `sys_irq_register`/`sys_irq_wait`, kernel.cpp hook,
  task cleanup drain, tests, docs.
- **Rules:** `prompts/CODING_STYLE.md` §11 (concurrency), §12 (lifecycles),
  §5.1 (ENSURE), §6 (ISR/debug parity); audit scope items 1–6.
- **Reference patterns:** `Semaphore::wait()` (blocking-wait),
  `Notify::notify/wait` (wake discipline), `KernelObject` (refcount),
  `Scheduler::reschedule/set_task_ready` (deferred switch).

## Verdict

**REJECTED** — two S1 capability/liveness defects and two S2 invariant
violations in the delivery-table ownership model. Corrective patch written to
`audits/rejected_patch.diff` (applies cleanly, `git apply --check` green).

## FINDINGS

### [S1] 1 — `sys_irq_register` re-arms a stale slot index without ownership revalidation (cross-vector hijack)

- **Files/lines:** `src/kernel/syscall/syscall_handlers_irq.cpp` (arm call at
  `irq->reg_idx_`), `src/kernel/irq_delivery.cpp` `IrqDelivery::arm()`.
- **Violated rule:** scope item 2 (slot-reuse safety, stale `reg_idx_` after
  drain); CODING_STYLE §12.5 (ownership checks on every state-changing op).
- **Justification:** `arm()` only checks `occupied && !armed`, never that the
  slot at `reg_idx_` still carries **this** cap's vector. IrqCap is
  `is_shared()` (grantable). After the arming task's death, `drain_task`
  clears the slot but a granted (shared) copy of the cap keeps a stale
  `reg_idx_`; when that slot is later claimed by a **different** vector, a
  register on the stale cap arms the wrong slot — the stale holder becomes
  recipient of another cap's vector and the legitimate owner is locked out.
  The patch itself guards dispose/revoke against "slot drained at task death
  and reused" via `slot_matches`, but **not** the register path.

### [S1] 2 — `sys_irq_wait` post-wake block re-locks a possibly-reused slot (lost wakeup / cross-vector delivery)

- **Files/lines:** `src/kernel/syscall/syscall_handlers_irq.cpp` post-wake
  block (`r->pending`/`r->armed` read + unconditional `r->waiter = nullptr`).
- **Violated rule:** scope item 1 (wakers own the wakeup contract — a blocked
  waiter must never be left BLOCKED forever); CODING_STYLE §12.3; scope
  item 2 (slot reuse).
- **Justification:** `r` is captured by `find(vector)` **before** blocking.
  While the task sleeps the slot can be released (revoke/dispose/drain) and
  reused by a **different** cap/vector. On wake the code re-locks the stale
  slot: it may consume the **new** slot's pending (returns the old vector —
  cross-vector delivery) and unconditionally clears `r->waiter`, unregistering
  the **new** slot's legitimate waiter, which is then never woken (lost
  wakeup → BLOCKED forever).

### [S2] 3 — `isr_entry` consumes a claimed-but-unarmed slot, contradicting the documented fall-through contract

- **Files/lines:** `src/kernel/irq_delivery.cpp` `IrqDelivery::isr_entry()`.
- **Violated rule:** scope item 3 (ISR routing correctness); the patch's own
  `kernel.cpp` hook comment ("Unarmed vectors fall through to the generic
  handler").
- **Justification:** `isr_entry` returns `true` (EOI + early return + `++pending`)
  for any **occupied** slot, even when `armed == false`. A user claiming a
  kernel-handled vector (e.g. RTC 40) without ever arming silently EOI-acks and
  swallows its IRQs, starving the generic handler for that line. The
  implementation contradicts the hook comment added by the patch itself.

### [S2] 4 — `arm()` mutates slot state without the per-slot SpinLock

- **Files/lines:** `src/kernel/irq_delivery.cpp` `IrqDelivery::arm()`.
- **Violated rule:** `irq_delivery.hpp` documented invariant ("All IRQ-state
  transitions happen under the slot's SpinLock"); CODING_STYLE §11.6.
- **Justification:** every other transition (`isr_entry`, `release_slot_idx`,
  `drain_task`, `slot_matches`, `sys_irq_wait`) serializes on `r->lock_`;
  `arm()` reads/writes `armed`, `recipient`, `recipient_gen` and drives PIC
  mask state unlocked, racing with the ISR wake path and teardown paths.

### [S3] 5 — duplicate `#pragma once`

- `src/kernel/cap/irq.hpp` (lines 180–181) and
  `src/kernel/irq_delivery.hpp` (lines 499–501) each carry `#pragma once`
  twice. Harmless but non-conformant (CODING_STYLE §3).

### [S3] 6 — missing trailing newline

- All six new/patched files end without `\n`. Cosmetic; fixed in the
  corrective patch.

## Assessment of remaining scope items (no finding)

- **Spinlock across `reschedule()`:** not held — `sys_irq_wait` mirrors
  `Semaphore::wait()` (register waiter under lock → release → `dequeue_ready`
  → `reschedule`); BLOCKED task is dequeued (INV-2) before `reschedule`.
- **Wake discipline / generations:** ISR and `release_slot_idx` wake paths
  reject `TERMINATED`/`REAPED` and verify `waiter_gen` (Notify discipline);
  `drain_task` never feeds a dying task to `set_task_ready`.
- **Refcounts:** `create`/`install`/`lookup`/`release`/`dispose` balanced on
  all syscall paths; single `MemPool::free` in `dispose`; `revoke`+`dispose`
  cannot double-release the slot (`reg_idx_` reset).
- **ENSURE:** none used for reachable states — exhaustion/validation all fail
  closed with `-1`/`nullptr` (CODING_STYLE §5.1).
- **ISR safety / EOI:** `isr_entry` is allocation-free, non-blocking, EOI
  exactly once (early return skips tail EOI); pending is recorded under the
  lock before waking.
- **Syscall validation:** `cap::lookup` type/rights/gen enforced; recipient==
  waiter check present; non-x86_64 returns `-1`. (Checks themselves are sound;
  the ownership hole is in items 1–2 above.)
- **Debug/release parity, no `const_cast`:** conformant.

## Corrective patch (`audits/rejected_patch.diff`)

Machine-applicable (`git apply --check` passes), 4 files:

1. `src/kernel/irq_delivery.hpp` — add `cap::IrqCap *owner` to
   `IrqRegistration`; add `set_slot_owner()` and `slot_belongs_to()`; drop the
   duplicate `#pragma once`.
2. `src/kernel/irq_delivery.cpp` — `claim_slot` clears `owner`;
   `set_slot_owner`/`slot_belongs_to` implemented under the slot lock;
   `arm()` takes the slot lock; `isr_entry` returns `false` when the slot is
   unarmed or no longer carries the delivered vector (falls through, tail EOI
   fires once); `release_slot_idx`/`drain_task` clear `owner`.
3. `src/kernel/cap/irq.cpp` — `create()` binds the slot owner;
   `dispose()`/`revoke()` use `slot_belongs_to` instead of `slot_matches`.
4. `src/kernel/syscall/syscall_handlers_irq.cpp` — `sys_irq_register`
   requires `slot_belongs_to(reg_idx_, irq)` before `arm()`; both lock scopes
   of `sys_irq_wait` revalidate `occupied && vector == irq->vector &&
   owner == irq && recipient == t`, and the wake path clears the waiter only
   when `r->waiter == t`.

After applying: rebuild (`make build`) and re-run `make execute-test x86_64
debug cap_irq` (and the `all` class once green), then re-audit.

## DECISION: REJECTED