# SIL 3 Safety Audit — Issue #107 (External Pager Protocol)

- **Auditor:** independent SIL 3 audit agent
- **Date:** 2026-09-04T13-07:47Z
- **Target:** `audits/pending_patch.diff` (19 files, `git diff` vs `main`)
- **Design authority:** `docs/specs/external-pager.md` (in patch)
- **Branch audited:** `main` (working tree carries the patch)
- **Runtime evidence:** fresh `make build` + `make execute-test x86_64 debug cap_pager` (this session)

---

## FINDINGS

### F1 — S1 — Committed `SYS_PAGER_MAP` cap pins are never released; client teardown frees the pager's frames (refcount leak + double-free / stale-aliasing)

**Location:** `src/kernel/ipc/pager_registry.cpp` — `map()` phase 3 (commit), `drain_task()`, `abort()`, `watchdog_scan()`, `invalidate_cap()`, `delegate_fault()`, `unregister()`, `snapshot_reset()`.

After a successful `map()` the commit path (`map()` phase 3, lines 643-650) sets `sl.pending = false` and `map_in_progress = false` **but leaves the fault ledger populated** (`mapped_va[]`, `pin[]`, `mapped_count`) with the `fc->acquire()` references taken at lines 621-640. From that point on **no code path releases those pins**:

| Path | Gate | Effect on committed pins |
|---|---|---|
| `abort()` | `!sl.pending` → skip (pager_registry.cpp:258) | not released |
| `watchdog_scan()` | `!sl.pending` → skip (:686) | not released |
| `invalidate_cap()` | `!sl.pending` → skip (:300) | not released |
| `drain_task()` | rollback only inside `if (sl.pending)` (:357); non-pending slot freed at :370 | not released |
| `unregister()` | rollback only inside `if (sl.pending)` (:201) | not released |
| `delegate_fault()` | next fault does `sl.fault = {}` (:526) — **drops the `pin[]` pointers without `release()`** | refcounts leaked |
| `snapshot_reset()` | rollback only for pending (:400) | not released |

Consequences:
1. **Refcount / memory leak.** The `FrameCap` refcount stays elevated (slot + leaked pins), so `dispose()` never runs and the wrapped PMM frames are never returned. Under sustained pager activity this is unbounded kernel-heap/PMM exhaustion.
2. **Client teardown frees the pager-owned frame (ownership violation → double-free / stale alias).** Because the committed ledger PTE is never unmapped by `drain_task` (it only rolls back pending records), the USER leaf PTE at `fault_va` remains in the client's PML4. `free_user_pages` (task.cpp:1874) frees every USER-owned leaf page — including the pager's frame (verified: `map_page_in_pml4` requires `PMM::is_user_page(phys)` for user mappings, vmm.cpp:604-606; the test frame is `alloc_user_contiguous` → user-owned). The pager's `FrameCap` keeps pointing at a freed page: when the cap is later disposed it `PMM::free_page`s the (re-)allocated page a second time, or aliases a page now owned by someone else. This violates exactly the invariant the drain-before-`free_user_pages` ordering (task.cpp:1863-1866, paper §5.3) was written to enforce.

**Runtime confirmation (this session, `cap_pager` 13/13 "PASS" but with RESOURCE WARN):**

```
[WARN] [RESOURCE] test=pager_fault_roundtrip | MemPool[2]   | before=6  | after=7  <---
[WARN] [RESOURCE] test=pager_fault_roundtrip | MemPool[7]   | before=0  | after=1  <---
[WARN] [RESOURCE] test=pager_fault_roundtrip | Cap objects  | before=0  | after=2  <---
[WARN] [RESOURCE] test=pager_fault_roundtrip | Cap slots    | before=0  | after=1  <---
[WARN] [RESOURCE] test=pager_fault_roundtrip | Pager maps   | before=0  | after=1  <---
```

The roundtrip's PMM delta is **0**: the `alloc_user_contiguous(1)` frame was freed by the client's `free_user_pages` (the pager's frame), not by the cap — the frame is freed while the cap still owns it.

---

### F2 — S1 — `delegate_fault` blocks on `s_lock_` from the #PF ISR: unrecoverable spin deadlock (the same class the watchdog `try_lock` was created for)

**Location:** `src/kernel/ipc/pager_registry.cpp:483-484` (`SpinLockGuard<sync::SpinLock> guard(s_lock_);` inside `delegate_fault`), called from the #PF ISR (kernel.cpp:1533-1537).

Verified facts:
- Vector 14 (#PF) is an **interrupt gate** (0x8E, `idt.cpp:66`): the #PF ISR runs with **IF=0**.
- Vector 0x80 (syscall) is a **trap gate** (0xEE, `idt.cpp:69-71`): syscall handlers run with **IF=1** (task context, preemptible).
- `s_lock_` is a plain busy-wait `SpinLock` (`spinlock.hpp:43-49`) — no IRQ disable.
- The pager syscalls (`register`/`recv`/`map`/`abort`/`unregister`) acquire `s_lock_` in **task context with IF=1**, so a timer tick can preempt the holder mid-critical-section (the exact premise the developer documented for the watchdog fix).

Scenario: a pager is preempted inside `map()` phase 1 (holding `s_lock_`); the timer's deferred switch runs a different user task; that task faults → #PF ISR (IF=0) → `delegate_fault` → `SpinLockGuard(s_lock_)` spins; the preempted holder can never be rescheduled while IF=0 → **permanent spin**. The patch's own "CRITICAL KNOWN FIX" acknowledges this hazard for `watchdog_scan` and fixes it with `try_lock` (verified correct at pager_registry.cpp:673-675) — but **the identical hazard was not fixed in the #PF ISR path**. This is a direct S1 omission.

---

### F3 — S2 — `SYS_PAGER_UNREGISTER` has no caller-authority check (third-party registration eviction)

**Location:** `src/kernel/ipc/pager_registry.cpp:187-225` (`unregister`), `src/kernel/syscall/syscall_handlers_pager.cpp:113-123`.

`unregister(client_pid)` matches slots only by `sl.client_id == client_pid`; there is **no check that the caller is the client or the designated pager**. Any user task can call `SYS_PAGER_UNREGISTER(victim_pid)` and (a) evict the victim's pager registration — the victim's next #PF reverts to SIGSEGV (availability DoS), and (b) consume + roll back the victim's pending fault and wake it, even if the victim is blocked mid-#PF. The in-code comment ("the caller may remove its own registration, or its pager may drop it (checked by slot match)") is false — no slot match against the caller exists. The registration path (`register_client`) correctly enforces self-only authority (pager_registry.cpp:149); the symmetric authority gate is missing on unregister.

---

### F4 — S2 — `SYS_PAGER_MAP` does not validate `count <= fc->count` (cross-capacity frame mapping)

**Location:** `src/kernel/ipc/pager_registry.cpp:573-574` (`count > CONFIG_PAGER_MAX_PAGES_PER_FAULT` is checked, but **`fc->count` is never compared with `count`**), phase-2 loop at :605-609.

A pager holding a 1-page `FrameCap` can pass `count=4` (≤ `CONFIG_PAGER_MAX_PAGES_PER_FAULT`) and map `fc->phys + 0..3*PAGE_SIZE` into the client's PML4 — three pages **outside the cap's extent**. The only guard (`PMM::is_user_page`, an unconditional `panic`, assert.hpp:48-54) only rejects kernel pages, not adjacent user-owned frames. A buggy or compromised pager can therefore grant its client access to any user-owned frame adjacent to its cap — a capability-mediation violation. The `sys_frame_map` precedent (`FrameUserMap`) bounds the mapping by the cap; the pager path does not.

---

### F5 — S2 — Test honesty: leaks are logged but never fail; counters are masked after every test

**Location:** `src/kernel/test/test_isolate.cpp:722-726`; new counters in `resource_tracker.{cpp,hpp}`; `test_cap_pager.cpp`.

`snapshot_restore` **discards the return value** of `ResourceTracker::check()` (line 722) and then unconditionally `restore(baseline)` (line 726), resetting all counters to the snapshot values regardless of whether resources actually leaked. So the new `any_leak` entries (pager_registrations/pager_faults/pager_mappings) fire as WARN-only and are masked for the next test. The developer's claim "zero-delta across tests" is **false** — this session's run shows **4 of 13** cap_pager tests leak:

- `pager_register_authority`: `Tasks +1`, `MemPool[8] +1`, `PMM +17`, MQ/Notify/EventGroup +1 (leaked parked task).
- `pager_fault_roundtrip`: `Cap objects +2`, `Cap slots +1`, `Pager maps +1`, `MemPool[2] +1`, `MemPool[7] +1` (F1 leak).
- `pager_map_after_timeout_denied`: `Cap objects +2`, `Cap slots +1`, `PMM +1`, `MemPool[2] +1`, `MemPool[7] +1` (test never removes its slot).
- `pager_cap_revoke_unmaps`: `Cap objects +1`, `MemPool[7] +1`.

The roundtrip/revoke tests additionally never exercise the live-ledger `invalidate_cap` path (a committed mapping + revoke), which is precisely the broken F1 case.

---

### F6 — S2 — `drain_task` does not defer `map_in_progress` records → unmap into a concurrently-freed PML4

**Location:** `src/kernel/ipc/pager_registry.cpp:332-381` (`drain_task`), vs. `watchdog_scan` which correctly defers `map_in_progress` records (:688-689).

`watchdog_scan` defers mid-map records (§7.2 TOCTOU pin); `drain_task` does **not**. If the client is terminated while the pager is between `map()` phase 1 and phase 3, `cleanup()`'s `drain_task(client)` consumes the record and frees the slot; `cleanup()` then proceeds to `free_user_pages` (task.cpp:1874). If the preempted pager reaches `map()` phase 3 (pager_registry.cpp:616-657) only after `free_user_pages` has run, its `unmap_frame_from_cap(fault_va, client_pml4)` writes into freed page-table memory (UAF). The `map_in_progress` pin protects against the watchdog but not against the death-drain.

---

## PATCH

**Verified-correct elements of the patch (not exhaustively re-stated):**

1. **Block-in-#PF-ISR mechanics (paper §4.8):** `switch_away_from_terminating` publishes `save_rsp_to = &task_stack_ptr(&exiting)` (scheduler.cpp:2820); the ISR epilogue stores the current ISR-frame RSP there (isr_stubs.asm:252) and iretq-resumes the target via `load_rsp_from` (isr_stubs.asm:289) — the exception frame is preserved and resume retries the faulting instruction. Only a `RUNNING` current is re-enqueued; a `BLOCKED` one stays dequeued (scheduler.cpp:2796-2799). `dequeue_ready` on a non-queued RUNNING task is a safe no-op (task_queue.cpp:94-98). `blocked_on_pager_fault` is set only at the block (pager_registry.cpp:502) and cleared only by the single `wake_client` (:78); it is never read for control flow. Exactly-once resume is enforced by the atomic PENDING→consumed gate (single consumer per record).
2. **Watchdog `try_lock`:** correct — a failed `try_lock` returns without unlock and safely skips the tick (pager_registry.cpp:673-675, 705); the 1000-tick deadline absorbs a skipped scan. Lock order `scheduler_lock_ → s_lock_` holds; no task-context path takes `scheduler_lock_` while holding `s_lock_` (the collect-under-lock / wake-outside-lock discipline is respected).
3. **Map-vs-watchdog TOCTOU (paper §7.2):** `map_in_progress` set/cleared under `s_lock_`; watchdog defers pinned records; phase 3 re-validates `PENDING` + `fault_id` + `!fc->revoked()` and rolls back phase-2 PTEs + partially-acquired pins on failure (counter-balanced). Correct as far as it goes (but see F6).
4. **Classification F1-F10:** every reject (`F2` recover-IP via `kernel::gs::user_access_recover_ip`; F3/F4/F5/F6/F7 bitmask checks; F8/F9/F10 under lock) returns **before** the block path; F10 canary-collision logic matches the TCB canary layout. `delegate_fault` has exactly one production caller (the #PF ISR, kernel.cpp:1533), so direct-call misuse is confined to the tests (which only exercise rejects).
5. **Cross-AS map authority:** gated solely by the pending fault record (caller == record pager, `fault_id` match); `cap::lookup(Frame, WRITE)` in the pager's CSpace (syscall_handlers_pager.cpp:1094); NX-only; no new VMM API. The `lookup` reference is released after `map()`.
6. **Syscall table integrity:** 69-73 with `MAX_SYSCALL=74`; none of the five pager syscalls are in `k_syscall_fast[]` (syscall.hpp:178-188); `SYS_PAGER_RECV` validates the destination `CheckedPtr` **before** consuming (syscall_handlers_pager.cpp:1062-1069) — no notification loss on a failed copy (the record stays pending).
7. **Cleanup/exec ordering:** `drain_task(client)` runs before `free_user_pages` (task.cpp:1866 vs 1874) and before the old PML4 is freed in `exec_into_current` (elf.cpp:724-728); `drain_task(pager)` runs before the `Notify` destructor (task.cpp:1866 vs 1888); `FrameCap::dispose()/revoke()` call `PagerRegistry::invalidate_cap` (frame.cpp:59, 71).
8. **Watchdog deadline:** B2 = 1000 ticks + one map window; per-fault fail-closed (registration KEPT, VA poison latch F9); eviction only on pager death/unregister.
9. **Build:** `make build` green; `cap_pager` runs 13/13 with a real ring-3 probe ELF faulting at 0x10000000 (non-vacuous) — though "PASS" is not a clean bill given F1/F5.

---

## DECISION

**REJECTED** — two S1 findings (F1, F2) and four S2 findings (F3-F6).

The `watchdog_scan` `try_lock` fix is verified correct, but the identical ISR-context deadlock remains in `delegate_fault` (F2), and the committed-map pin lifecycle leaves a refcount leak plus a client-teardown double-free of pager-owned frames (F1, confirmed at runtime). The patch must be reworked and re-audited. `audits/rejected_patch.diff` (git apply-able vs `main`) is written alongside this report.

DECISION: REJECTED