# IPC, Synchronisation & Deferred-Switch Contracts

**Semantics:** binding contracts for message-passing IPC, the sync primitives,
and the deferred context-switch machinery that carries IPC blocking.
Synthesis of `_archive/ipc-sync-audit-fix.md`, `_archive/ipc_blocking-analysis.md`
(H1 fixed, H2 resolved), `_archive/ipc_blocking-C-rootcause.md`, and the IPC
rows of `specs/scheduler.md`.  Audit fix status is code-verified where marked.

## 1. Message-Queue Model

```
 task A ── IPC::send(ty,msg) ──▶ q (MessageQueue) ── IPC::recv(...) ──▶ task B
             │  push(msg)                         pop(msg)  │
             ▼                                               ▼
        if q full + block:                     wake_sender → set_task_ready
        block_sender → BLOCKED + dequeue_ready
        PI boost: q.owner->priority = task.priority → move_priority
```

- Queue state (`push/pop`) is protected by `SpinLockGuard<sync::SpinLock> lock_`.
- `blocked_senders_head/tail/blocked_next` MUST also be mutated under `q.lock_`
  (VULN-IPC-02).  Lock ordering: `scheduler_lock_` first, then the queue lock.
- `is_full()` reads must be `is_full_locked()` (acquire the lock) to avoid a
  TOCTOU with a concurrent `push` (VULN-IPC-02).

### 1.1 Send-path rollback (VULN-IPC-01) [IMPLEMENTED]
When `IPC::send()` blocks while interrupts are disabled, `block_sender()` has
already mutated the caller's TCB (BLOCKED + dequeued + linked into the dest's
blocked-senders list).  The caller must roll back via
`unblock_sender_rollback()`:
1. remove `task` from `q.blocked_senders_head/tail`;
2. `blocked_on_queue = nullptr; blocked_next = nullptr;`
3. `state = RUNNING`;
4. `Scheduler::enqueue_ready(task)`.
Entry guard: `ENSURE(task.blocked_on_queue == &q)`.

## 2. `send_sync` Contract

**Semantics:** `send_sync` delivers a message and blocks until the reply arrives.

- **dequeue before block [CHANGED, IMPLEMENTED]:** `send_sync` MUST call
  `Scheduler::dequeue_ready(*cur)` immediately before `cur->state = BLOCKED`
  (VULN-IPC-03).  A BLOCKED task must NEVER have `in_ready_queue_ == true`
  (WEDGE orphan).  This supersedes the historical "BLOCKED without dequeue"
  INV-5 exception.
- **Reply-before-peer-gone (H1, FIXED):** if the peer terminates, `send_sync`
  may only `return false` when the sender's own reply queue is genuinely empty.
  A delivered reply (`IPC::send` → WAKE into the sender's queue) must be
  consumed by the existing `pop(reply)` — "reply in own queue ⇒ success".
- **Spin-wait:** the reply wait is a `hlt()` loop (not a bare pause-spin).

```
 peer terminated? ── no ──▶ wait for reply
      │
      ▼ yes
 reply in own queue? ── yes ──▶ pop(reply), success
      │
      ▼ no
 return false
```

## 3. Mutex / PCP Contract

- `Mutex::lock()` (void overload): on PCP retry exhaustion
  (`MAX_WAITERS + 1` attempts), it MUST `panic()` rather than silently return
  unlocked (VULN-SYNC-01).  The `_err` overload returns `SYNC_ERR_INTERRUPTED`.
- Waiter arrays should carry a TCB `generation` cookie (proven in `BufferPool`)
  to defeat stale-handle reuse (VULN-SYNC-03).

## 4. Deferred-Switch Machinery (carrier for IPC blocking)

**Semantics:** the scheduler never switches inside the blocking syscall; it
publishes a deferred-switch pair that the next timer-ISR epilogue applies.

```
switch_to_task (publisher, task ctx)            isr_stubs.asm (applier, ISR)
─────────────────────────────────────           ──────────────────────────────
  resolve RSP owner (INV-1)                        capture generation
  validate next iret frame (rsp ∈ kstack)          re-verify generation
  publish: load_rsp / load_cr3 / kstack_base/top   save old RSP → save_target
  bump generation  (RELEASE)                       load new RSP
  arm save_rsp_to                                  verify new RSP ∈ kstack   ← H2 L6
  (scratch-save if current on orphaned stack)      iretq
```

**H1 (send_sync reply loss) — FIXED** as described in §2.
**H2 (deferred-switch race) — RESOLVED 2026-08-13/15** (final fix commits
`71b3a088`, `4bf751b4`, `b85ba27d`; the earlier 2026-08-05 claim was
premature — the three layers below contain but did not eliminate it):
1. **Dispatch-guard frame.rsp validation** (C++): a ring0 task's iret-frame
   `rsp` field must lie in its own `[kernel_stack, kernel_stack_top]` (or the
   boot stack for the harness) before it may be dispatched.
2. **Scratch-save healing:** when the current task runs on an orphaned/foreign
   stack, the save writes the foreign RSP to a scratch (`s_foreign_rsp_scratch`)
   instead of corrupting `context.rsp`, so the next dispatch re-plants it.
3. **Apply-side RSP-owner check (asm):** `isr_stubs.asm` verifies the loaded RSP
   is inside `[scheduler_load_kstack_base, scheduler_load_kstack_top)` before
   `iretq`, aborting any stale/foreign load.

**RESIDUAL — RESOLVED 2026-08-13/15** (commits `71b3a088` — stale-resume
orphan re-enqueue; `4bf751b4` — owner-resolution self-switch no-op;
`b85ba27d` — elf_loader `lock_` held across a timer-ISR preemption).  Debug
`all` gates pass with the trace OFF (873/873 ×2, 932/932, 942/942).

## 5. Sync Audit Findings (status ledger)

| ID | Finding | Status |
|---|---|---|
| VULN-IPC-01 | send() IRQs-disabled rollback omission | fix spec, rolled back in code |
| VULN-IPC-02 | unguarded blocked_senders list | fix spec (needs queue lock) |
| VULN-IPC-03 | send_sync missing dequeue_ready | **IMPLEMENTED** (ipc.cpp:301) |
| VULN-SYNC-01 | Mutex::lock silent PCP-retry failure | fix spec (panic) |
| VULN-SYNC-02 | (waiter-array reuse) | see VULN-SYNC-03 |
| VULN-SYNC-03 | TCB generation cookie for waiter arrays | fix spec (BufferPool-proven) |

## 6. Gaps

- **Priority-ordered blocked-sender wakeup** (rms-rework Plan Phase 3) — not
  confirmed implemented; `wake_sender` pops FIFO.
- **`sys_receive` bounded/timeout variant** (VULN-W3 in `specs/boundary.md`) —
  uses the discarded `arg3` slot as `timeout_ticks`; verification status
  unconfirmed.
