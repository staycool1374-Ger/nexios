# ipc_blocking Flakiness — Root-Cause Analysis (2026-07-18)

Single-core x86_64 kernel. `ipc_blocking` test class has **two distinct failure
modes**. One is FIXED; the other (H2) was characterized here and is
**RESOLVED 2026-08-13/15** (see the final fix section below).

## FIXED: H1 — send_sync reply loss (caused `LEAK: Tasks -3` FAIL)

- **Symptom:** test 2 `ipc_send_sync_was_blocked_restores_state` intermittently
  FAILs with `LEAK: Tasks -3`.
- **Root cause:** `IPC::send_sync` (`src/kernel/ipc/ipc.cpp:259-265`) bails with
  `return false` when the peer is `TERMINATED`, *before* re-checking its own
  reply queue. The receiver's normal lifecycle terminates *after* delivering its
  reply (`IPC::send` → `WAKE` into sender's queue). So a delivered reply was
  discarded and `send_sync` reported failure.
- **Proof (deterministic trace):**
  ```
  [SEND] to=7 from=6 ty=99 q=1
  [WAKE] dest=7 st=2 inrq=0 q=1        # reply queued into sender(7)
  [SYNC-FAIL] dest-gone-reply cur=7 dest=6 q=1   # bail despite queued reply
  S: ... 2/4: FAIL [LEAK: Tasks -3]
  ```
- **Fix (F1, committed-in-tree, uncommitted):** when peer gone, only `return
  false` if the queue is genuinely empty; otherwise `break` and let the existing
  `pop(reply)` consume the delivered reply. Honors the IPC contract (reply in
  own queue ⇒ success).
- **Verification:** 0 FAIL across 20+ runs after the fix (was ~1/8 before).

## H2 — deferred-switch / userspace-task crash in test 3 & 4 (HANG — RESOLVED 2026-08-13/15)

Two HANG subtypes, both rooted in test 3/4's **userspace task** design +
the deferred context-switch machinery. ~45% of runs hang.

### Subtype A — SIGSEGV (task 6 user lambda)
- Task 6 (user task in `ipc_userspace_block_uses_sti_hlt_cli`) SIGSEGVs:
  `CR2=0xFFFF80000022C32F` (= kernel address of the test lambda `FUN`),
  `err=0x15` (user write to supervisor page), `CR3=0x1909000` (kernel PML4).
- `nm` maps `0xFFFF80000022C32F` →
  `_ZZ41test_ipc_userspace_block_uses_sti_hlt_clivENUlvE_4_FUNEv` — the kernel
  lambda used as the user task's entry point.
- `scheduler.cpp:[SW] next=6 cr3=26251264` shows the switch is set up with the
  CORRECT user PML4 (26251264), but the ISR applies **kernel PML4** (0x1909000)
  → mismatched RSP/CR3 pair. This is the split-phase deferred switch:
  `switch_to_task` writes `scheduler_load_rsp_from` and `scheduler_load_cr3_from`
  as two separate stores; a timer ISR firing between them (or the
  `next_is_runner` self-promotion path at `scheduler.cpp:1329` returning without
  re-publishing the pair) lets the ISR apply a stale CR3 with a new RSP.
- **Attempted fixes (all REVERTED — did not reduce hang rate):** (1) defuse
  stale trigger at start of `switch_to_task`; (2) spin-wait for pending switch to
  be consumed (made it WORSE — deadlock under IRQs-off `yield_as`);
  (3) consume pending switch in the `next_is_runner` early-return.

### Subtype B — live-lock / freeze (current_task_ptr_ lag + runq desync)
- **Captured deterministically in ONE run** (saved as
  `docs/ipc_blocking-c-baseline.log` via the `[STALL]` watchdog). After test 2
  PASSES, the system freezes; `[STALL] ticks_since_switch=7561 cur=1 bm_lo=0`
  with the full task dump:
  ```
    T5 st=2 inrq=0 rq=0 eff=127 pg=0          (TERMINATED)
    T2 st=2 inrq=0 rq=0 eff=80 pg=24305664    (TERMINATED)
    T3 st=2 inrq=0 rq=0 eff=70 pg=24494080    (TERMINATED)
    T6 st=1 inrq=0 rq=0 eff=11 pg=26251264    (RUNNING — user task, orphaned)
    T1 st=0 inrq=0 rq=0 eff=10 pg=0           (READY — harness = current)
    T0 st=0 inrq=0 rq=0 eff=0 pg=0            (idle)
  ```
- **Root cause (C, proven):** `bm_lo=0` (ready-queue bitmap EMPTY) yet TWO tasks
  are `RUNNING` (task 1 = harness = `current_task_ptr_`, and task 6 = the user
  task). Task 6 is RUNNING with `inrq=0` — it is the **physically-executing**
  task but `current_task_ptr_` points at task 1. Because `next_task()` excludes
  `current` (task 1) and the bitmap is empty, it returns idle forever; task 6 is
  never re-selected. This is the **`current_task_ptr_` lag** from the split-phase
  deferred switch (the ISR updates `current_task_ptr_` only when it applies the
  RSP swap, which never happened for task 6) combined with the runq being empty
  while a real RUNNING task exists (INV-2 violation: a live task is not in the
  runq and not `current`).
- The earlier "634,962 × `[SW] next=6` constant-RSP" observation was the same
  root manifesting as a tight live-lock (task 6 repeatedly selected) rather than
  a full freeze; both are the `current_task_ptr_`/runq desync.

### Underlying design issue
`create_user` (`task.cpp:379,392`) sets the user task's RIP/RDI to the
**kernel virtual address** of the C++ lambda. A true user task cannot execute
supervisor pages (Subtype A) and the way it "blocks" depends on `sys_receive`
internals that the lambda does not invoke (it calls `IPC::recv` directly, which
does NOT block). Whether the run SIGSEGVs (A) or live-locks (B) depends on the
active page-table permissions at the moment of dispatch — non-deterministic due
to the deferred-switch CR3 race. The **test infrastructure itself is suspect**:
a userspace task should reach `IPC` via a syscall wrapper, not call kernel
`IPC::send`/`IPC::recv` directly.

## Diagnostic harness (SAVED, reusable — `CONFIG_DEBUG_IPC_SCHED`)
- `src/kernel/debug/ipc_sched_trace.hpp` — printf-free serial trace macros,
  gated by `CONFIG_DEBUG_IPC_SCHED` (enabled). Toggle the `#define` to reuse.
- Traces emitted: `[SEND]`, `[WAKE]`, `[SYNC]`, `[SYNC-FAIL]` (with
  `dest-gone-empty` vs `dest-gone-reply` discrimination), `[SW]` (next/cr3/rsp),
  `[WEDGE]` (orphan scan: READY/RUNNING task with `in_ready_queue_=1` but not
  physically linked).
- `TaskQueue::contains()` added for the orphan physical-link check.
- These are NON-INVASIVE (trace-only); safe to leave enabled.

## Next steps (for the hang / H2)
1. Decide whether test 3/4's user task should call IPC via a **syscall** rather
   than the kernel `IPC::` class directly. If the test is "correct by design",
   the kernel must make userspace entry execute properly (map kernel .text as
   user-accessible in cloned PML4, or copy the thunk to a user mapping).
2. Fix the deferred-switch RSP/CR3 atomicity: publish the pair under a single
   seqlock/generation so the ISR never applies a half-written pair. The ISR
   apply path is `src/kernel/arch/x86_64/isr_stubs.asm:106-171`.
3. Fix `in_ready_queue_` desync: ensure a BLOCKED task is unlinked from the runq
   (the `next_task()` lazy rebuild heals it, but the live-lock shows it is not
   being reached for task 6).

**2026-08-03 — additional root cause (harness preemption + priority direction):**
the H2-class live-lock also fires when a test task outranks the harness.  The
kernel's priority convention is **higher number = higher priority**
(`docs/scheduler-spec.md` §0).  The harness (init/PID 1) runs at prio 10 during
a test cycle; a sporadic task that is EXHAUSTED drops to `bg_priority_`, and if
that `bg_prio` is numerically HIGHER than the harness (e.g. base=10,
bg_prio=42) it preemptively dispatches the exhausted task mid-test.  Test code
must keep `bg_prio < base` and `< 10`.  Fixed: `SsExhaustionTriggersDeadline` /
`SsDeadlineMissDuringReplenish` bg_prio 42→2 (the harness-nonpreempt guard is
UNCHANGED — it keeps the `highest_ready < cur_prio` check; idle_cleanup relies
on equal/higher-priority dispatch, so the guard cannot return unconditionally).

**2026-08-03 — deterministic reproduction (ipc class):** `ipc` hangs 3/3 at
`ipc_send_sync_roundtrip` with the trace ON, ending in:
```
[DIAG] pre-save: idx=3 id=1 cur_rsp=0xFFFF800000A23EA8 ctx_rsp=0xFFFF900000034920
                 state=0 kstack=[0xFFFF900000025000-0xFFFF900000035000] owners:
```
i.e. the harness (PID 1) physically runs on the boot stack
(`0xFFFF8000...`, kernel-image space), which is not any TCB's kernel stack, so
`switch_to_task`'s owner-resolution finds no owner.  A timer ISR applying the
deferred switch then saves the boot-stack RSP into a peer TCB's `context.rsp`
and/or resumes the harness with a stale/wrong CR3.

**2026-08-03 — attempted fixes (ALL REVERTED, none stable):**
- (a) harness-slot fallback in `switch_to_task` owner-resolution (no-owner ⇒
      save into the harness TCB) — did not reduce the ipc hang.
- (b) early-return in `rate_monotonic_schedule` when a deferred switch is
      pending — no change.
- (c) clear `scheduler_next_task_id` in `remove_task` (cancel a pending switch
      to a removed task) — changed the ss_deadline manifestation, did not fix.
- (d) harness-nonpreempt guard returning unconditionally while the harness is
      RUNNING in a test body — fixed ss_deadline but broke
      `idle_cleanup`/`timer_rate_monotonic` (equal-priority RT tasks never
      dispatched; they must be able to preempt the harness during hlt()).

**Open question for the next session (CR3 on the harness-return path):** the
switch from the harness (boot stack, kernel PML4) to a user task loads the
user's CR3.  Switching BACK to the harness must reload the kernel PML4; if
`scheduler_load_cr3_from` for the harness is stale/zero, the harness resumes on
the sender's user PML4 → freeze.  Verify the CR3 reload on the return path
(isr_stubs.asm ~150-165) before/with the generation-based atomic publish fix.

---

## H2 RESOLVED (2026-08-05) — Investigation Log + Final Fix

> **Note (2026-08-13/15):** this 2026-08-05 resolution was premature — the
> residual race persisted (~17–50% `ipc`/`all` hang) until the final fixes
> `71b3a088` (stale-resume orphan re-enqueue), `4bf751b4` (owner-resolution
> self-switch no-op) and `b85ba27d` (elf_loader `lock_` across a timer-ISR
> preemption).  Debug `all` gates: 873/873 ×2, 932/932, 942/942 (trace OFF).

### Symptom (unmasked)
With the logging backend routed through the QEMU debugcon (0xE9, no UART
latency — see `src/kernel/arch/qemu_debugcon.hpp`), the `ipc` class hung at
test 21 `ipc_send_sync_roundtrip` with:
```
[DIAG] pre-save: idx=2 id=1 cur_rsp=0xFFFF800000A1BEA8 ctx_rsp=0xFFFF900000032920
                 state=0 kstack=[0xFFFF900000023000-0xFFFF900000033000] owners: (empty)
[DIAG] tasks: {0 idle kst=0xFFFF900000001000} {2 vfsd kst=0xFFFF8000007AF000}
               {3 iocd kst=0xFFFF8000007D5000} {1 harness st=0 inrq=1 pr=10
               rsp=0xFFFF900000032920 kst=0xFFFF900000023000}
```

### Root cause chain (all confirmed with instrumentation)
1. **Harness displacement:** from ~tick 19 (boot, during init_task_main's daemon
   wait) the harness (PID 1) physically executes on an **orphaned stack**
   `0xFFFF800000A1B000` (a PMM page ~10 MB, owned by NO TCB — the pre-save owner
   scan is empty; the boot DIAG-TABLE shows no task's kernel_stack there).  Its
   TCB `kernel_stack` field still says the kslot window `0xFFFF900000023000`.
2. **Snapshot drift:** `restore_task_fields` (scheduler.cpp ~2259) restores
   `context` (incl. `context.rsp`) from the snapshot baseline on every
   `snapshot_restore`, but the **physical RSP register stays on the orphaned
   stack**.  So the harness's stored context is a valid kslot frame
   (`rip=arch_hlt`, `rsp=0xFFFF9000000329D0`) while it keeps running foreign.
3. **First-preemption corruption:** the harness is preempted while foreign; the
   pre-save fires and the ISR save (`mov [save_target], rsp`) writes the
   orphaned RSP into `context.rsp`.  The dispatch-guard then rejects the
   now-foreign frame → the harness is stranded in the ready queue → idle-loop
   hang.

### Displacement-source investigation (exhausted)
- **Every** of the 43 observed harness dispatches lands it on its kslot
  (`H2-DISP1`: live_rsp and frame.rsp both kslot) — so the move is NOT a
  normal deferred-switch dispatch.
- No `mov rsp` instruction exists in kernel code except `higherhalf_entry`
  (boot) and `reboot_from_table`'s idle-stack handoff.
- The `syscall_entry` GS-based stack switch (`mov [gs:0x00], rsp; mov rsp,
  [gs:0x08]`) is **dead code**: userspace uses `int $0x80`
  (`src/libc/syscall.h:82` → `isr_128` → `isr_common`), and no
  `IA32_KERNEL_GS_BASE` (0xC0000102) MSR is ever written, so `swapgs` would
  #PF to phys 0.
- All frame-RSP writers (`deliver_signal_to_user` regs[20], `sys_sigreturn`
  regs[20]) are USER-task-only (`page_table_ != 0`), never the ring0 harness.
- The residual mechanism is an extremely narrow boot-time window (a single
  per-tick instruction perturbs it — the H2-FOREIGN per-tick check made the
  race vanish 25/25).  It requires a hardware-watchpoint session (lldb DR0–3
  on `&context.rsp` or the orphaned page) during an unperturbed run to pin the
  exact instruction; the QEMU `-icount rr=record/replay` path was impractical
  (firmware boot replays too slowly).

### Fix (three layers, committed)
1. **Dispatch-guard iret-frame `rsp` validation** (`switch_to_task` +
   `switch_away_from_terminating`): a ring0 task's frame `rsp` field must lie
   within its own `[kernel_stack, kernel_stack_top]` (inclusive top — a fresh
   task's create-frame carries `rsp == kernel_stack_top`), or within the linker
   boot stack when dispatching the harness.  A stale/foreign `rsp` (a freed
   test-task's HHDM stack) now rejects the switch instead of iretq'ing the task
   onto foreign memory.
2. **Scratch-save healing:** when the current task is detected on an
   orphaned/foreign stack, the ISR save writes the foreign RSP to
   `s_foreign_rsp_scratch` instead of `context.rsp`, keeping the valid kslot
   frame intact so the next dispatch re-plants the task on its own stack.
3. **Apply-side RSP-owner check** (`isr_stubs.asm`): new atoms
   `scheduler_load_kstack_base/top` published with each switch; the ISR verifies
   the loaded RSP lies within the dispatched task's kernel stack BEFORE iretq,
   aborting (restoring the old RSP, clearing the atoms, dropping the switch) any
   stale/foreign load — the split-phase/nested-ISR mismatch the C++ guard cannot
   see at publish time.

### Verification (clean build, `CONFIG_DEBUG_IPC_SCHED` OFF)
| Gate | Result |
|---|---|
| `ipc` | 5/6 clean (residual ~17% narrow boot-time race remains) |
| `scheduler` / `ipc_blocking` / `ipc_robustness` | 63/63, 4/4, 6/6 |
| `all` | **tests 1–347 PASS — H2 hang at test 77/78 GONE**; freezes at test 348 `timer_deadline_miss_detection_fires` (PRE-EXISTING timing-cluster bug, verified at baseline) |
| `make build` | check-style Errors: 0 |

### Remaining failures (all pre-existing, NOT H2)
- `all` test 348 / `timing` class: `timer_deadline_miss_detection_fires` freezes
  (MemPool pinned-block cleanup skip `LEAK: Tasks +1, PMM +16, ...` at
  `timer_period_reload`; deadline-monitor interaction).
- `priority_inheritance` test 1 `MutexPriorityDonates`: INV-4 gate-spin
  test-code race in `spawn_holder` (self-terminates before the harness observes
  BLOCKED).
- `ipc` residual ~17% hang from the narrow H2 boot-time window — **RESOLVED
  2026-08-13/15** (`71b3a088` stale-resume orphan re-enqueue, `4bf751b4`
  owner-resolution self-switch, `b85ba27d` elf_loader lock-across-preemption);
  debug `all` gates pass with the trace OFF (873/873 ×2, 932/932, 942/942).
