# SIL 3 Safety Audit — Issue #107 (External Pager Protocol) — Iteration 2

- **Auditor:** independent SIL 3 audit agent
- **Date:** 2026-09-04T13-37-17Z
- **Target:** `audits/pending_patch.diff` (19 files, `git diff` vs `main`, iteration-2 rework)
- **Design authority:** `docs/specs/external-pager.md` (in patch)
- **Branch audited:** `main` (working tree carries the patch)
- **Prior decision:** iteration 1 (report-2026-09-04T13-07-47Z) REJECTED — 2× S1 (F1, F2) + 4× S2 (F3–F6)
- **Runtime evidence:** fresh `make build` green; `make execute-test x86_64 debug cap_pager` 13/13 PASS (test=2223 ms, Pager counters zero-delta); `make execute-test x86_64 debug cap_shm` 5/5 PASS (baseline comparison). This session.

---

## FINDINGS

### Previous-finding remediation (verified against the full patch + runtime)

| ID | Sev | Finding (iter 1) | Remediation | Verdict |
|---|---|---|---|---|
| F1 | S1 | Committed `SYS_PAGER_MAP` pins never released → refcount leak + client teardown double-frees the pager's frame | Slot gains a bounded committed table (`mapped_va/mapped_pml4/mapped_pin/mapped_count`, ≤ `CONFIG_PAGER_MAX_COMMITTED_PAGES=16`). `map()` phase 3 acquires pins and commits; `drain_task`/`unregister`/`invalidate_cap`/`snapshot_reset` release them (collected under `s_lock_`, released OUTSIDE it — `release_committed` → `dispose` → `invalidate_cap` re-enters `s_lock_`). Client teardown (`task.cpp:1866`) drains before `free_user_pages` (`task.cpp:1874`); `exec_into_current` drains before the old PML4 is freed (`elf.cpp:724`). | **FIXED** (double-free + leak gone; runtime: roundtrip's prior `MemPool[2]+1` / `Pager maps +1` leaks no longer appear). **A closure defect remains — see N2.** |
| F2 | S1 | `delegate_fault` spins on `s_lock_` in the IF=0 #PF ISR (unrecoverable deadlock) | `delegate_fault` uses `s_lock_.try_lock()` and FAILS CLOSED (SIGSEGV) when the lock is held; every reject path unlocks before returning. | **FIXED** — no blocking `s_lock_` acquisition in any ISR-reachable path (`delegate_fault` try_lock, `watchdog_scan` try_lock/skip-tick). |
| F3 | S2 | `SYS_PAGER_UNREGISTER` had no caller-authority | `unregister(caller, client_pid)`: caller must be the client itself (`caller.id == client_pid`) or the designated pager (slot `pager_id/pager_gen` match); third-party eviction rejected. | **FIXED** |
| F4 | S2 | `SYS_PAGER_MAP` never validated `count <= fc->count` | `map()` rejects `count > fc->count` before any VMM work. | **FIXED** |
| F5 | S2 | Leak-masked harness + real leaks (4/13 tests) | Pager-specific counters (`pager_registrations/pager_faults/pager_mappings`) are now zero-delta. Iter-1 leaks (test 1 `Tasks+1/MemPool[8]+1/PMM+17/MQ+1`; roundtrip `Cap+2/Cap slots+1/PMM+1`; map-after-timeout `Cap+2/Cap slots+1/PMM+1`) are GONE. | **FIXED** — the residual `Cap objects +1 / MemPool[7] +1` in tests 5/6/11 is the harness's `ensure_cspace()` CNode (2048-B pool), verified identical to cap_shm's own `frame_user_map_revoke_denied` (this session: same `MemPool[7]+1 / Cap objects+1`). Accepted pre-existing baseline. |
| F6 | S2 | `drain_task` didn't defer `map_in_progress` → unmap into a concurrently-freed PML4 | `map()` runs ALL phases (1–3 + wake) under `arch::IrqGuard` (`pager_registry.cpp:613`), making the map atomic w.r.t. the scheduler: no tick → no deferred switch → no termination mid-map; `watchdog_scan` also defers `map_in_progress` records. Nested `IrqGuard` with `set_task_ready`'s own guard is safe (HAL guard saves `irq_was_`, inner destructor does not re-enable). | **FIXED** |
| — | — | — | `abort()`/`watchdog`/`drain` can all race `map()` only if the pager were preemptible mid-map; `IrqGuard` renders those consumers impossible during the map window. | **FIXED** |

### N1 — S1 — `drain_task`'s collect Work array overflows the stack (8 slots × 17 entries = 136 > 128)

**Location:** `src/kernel/ipc/pager_registry.cpp:311` — `Work work[kMaxClients * CONFIG_PAGER_MAX_COMMITTED_PAGES];` with `kMaxClients = CONFIG_CAP_MAX_PAGER_CLIENTS = 8`, `CONFIG_PAGER_MAX_COMMITTED_PAGES = 16` → array holds **128** entries.

Per matched slot, `drain_task` collects:
- `sl.mapped_count` committed entries (`≤ 16`, enforced by `map()` phase-1 check `sl.mapped_count + count > CONFIG_PAGER_MAX_COMMITTED_PAGES` → `-1`), **plus**
- **one** pending record (`if (sl.pending)`, `pager_registry.cpp:338-345`).

⇒ one slot contributes up to **17** entries. A single dying/exec'ing task can match up to **8** slots (pager of 8 clients; or pager of 7 + client of 1). Worst case `n = 8 × 17 = 136 > 128` → **stack buffer overflow by up to 8 `Work` entries (~320 B past the array)**.

- A slot genuinely reaches `mapped_count == 16` *and* `pending == true`: committed entries persist across completed faults (16 pages via 4×4 or 16×1 maps), and a *new* fault creates a fresh pending record without touching the committed table (the client is BLOCKED during the pending fault, so the table cannot change).
- Reachability: any pager that dies (`cleanup → drain_task`, `task.cpp:1866`) or execs (`exec_into_current → drain_task`, `elf.cpp:724`) while serving heavily-paged registrations. `exec` is a normal, user-triggerable operation.
- Consequence: structured writes past the array clobber adjacent stack state (including `n` if the compiler places it after `work`); the follow-up release loop then iterates garbage `work[128..135]` and releases bogus cap pointers → wild `release()`/`unmap` → crash or memory corruption. Deterministic memory-safety defect in the F1 remediation.

**Fix (in `audits/rejected_patch.diff`):** size the array `kMaxClients * (CONFIG_PAGER_MAX_COMMITTED_PAGES + 1)` (= 136), which bounds the true per-slot maximum of 17. Verified to compile and to reproduce the intended result when applied.

### N2 — S2 — `invalidate_cap` removes only ONE entry per slot per call → revoke does not close multi-page (or repeated) mappings; pin leak

**Location:** `src/kernel/ipc/pager_registry.cpp:252-296` — inner scan `break`s after the first `sl.mapped_pin[j] == fc` match inside a slot; the outer loop moves to the *next slot*.

A slot can hold **several** entries pinned to the same `FrameCap`:
- a single `SYS_PAGER_MAP` with `count` 2..4 maps `count` pages, **all** committed with `mapped_pin[·] = fc`; or
- the same cap mapped for repeated faults of one client.

On `FrameCap::revoke()/dispose()` → `invalidate_cap(fc)` only the *first* such entry is swapped-out/unmapped/unpinned; the remaining entries keep their PTE in the client's PML4 and their pin. They are released only later by `drain_task`/`unregister`/`snapshot_reset` (client death or registration removal).

Consequences:
1. **Broken revocation closure:** a revoked cap's pages remain accessible to the client until the client dies — a capability-mediation failure (revoke must withdraw the granted access).
2. **Pin leak:** the cap's refcount stays elevated, so the pager's frames cannot be reclaimed by the pager after revoke (bounded per slot by the committed-table cap, but unbounded across slots/clients over time).

No double-free / UAF (the leftover pins keep the frames alive), hence **S2**, not S1.

**Fix (in `audits/rejected_patch.diff`):** remove *all* matching entries per slot (collect under the lock, release outside it; re-examine the swap-in at index `j`).

### S3 notes (non-blocking, no change demanded)

1. `drain_task` pager-death path calls `wake_client` once per committed entry + pending record for the same client. Benign: `TaskQueue::push_back` refuses a second enqueue via `in_ready_queue_`.
2. `map()` does not validate `fc->is_user`; a FrameCap wrapping a kernel-owned frame would hit `PMM::is_user_page`'s unconditional panic in `map_page_in_pml4`. Fail-stop, not silent — hardening suggestion: reject non-user caps at `map()` with `-1`.
3. `delegate_fault` pulses `p->notify.notify()` in the #PF ISR, briefly taking `notify.lock_`. Pre-existing ISR-notify pattern (`irq_delivery.cpp:256/309/351`); `Notify::wait()` explicitly releases the lock before `reschedule` (notify.cpp:115-119) and all holds are brief. Accepted discipline; a missed pulse is harmless (RECV polling is authoritative).
4. `snapshot_reset`'s `Work work[128]` exactly fits the 8×16 committed bound (no pending entries collected) — correct today, but the same fragility N1 exposed; worth the `+1` in a future cleanup.

---

## PATCH

**Verified-correct elements (not exhaustively re-stated):**

1. **Committed-pin lifecycle (F1 core):** pins acquired in `map()` phase 3; every release path collects under `s_lock_` and releases/wakes/unmaps outside it (`release_committed`/`release_all_committed`); `drain_task` runs before `free_user_pages` and before the pager's `Notify` destructor; `FrameCap::dispose()/revoke()` both call `invalidate_cap`. `map()`'s acquire-failure rollback counter-balances partially-acquired pins exactly.
2. **ISR lock discipline (F2):** `delegate_fault` `try_lock` + fail-closed; `watchdog_scan` `try_lock` + skip-tick; lock order `scheduler_lock_ → s_lock_` holds (no task path takes `scheduler_lock_` while holding `s_lock_`; `dequeue_ready`/`set_task_ready` are lock-free). `find_task`/`id_table_find` is a bounded lock-free table read, safe at IF=0.
3. **`map()` IrqGuard (F6):** guard spans phase 1 (claim) + phase 2 (VMM map) + phase 3 (revalidate + commit) + the wake; nested `IrqGuard` with `set_task_ready` is safe. `abort()`/`watchdog`/`drain`/`unregister` cannot interleave a mid-map pager.
4. **Exactly-once resume:** PENDING→consumed is a single atomic gate under `s_lock_`; consumers are exactly {`map` phase-3, `abort`, `watchdog`, `drain`, `unregister`}; late `MAP` after consume fails phase-1 (`!sl.pending`) with `-1` and no wake; `wake_client` revalidates id+generation.
5. **Classification F1–F10:** F2–F7 reject before the lock; F8/F9/F10 under `try_lock` with explicit unlock+reject; every reject precedes the block path (`BLOCKED` + `dequeue_ready` + `switch_away_from_terminating`).
6. **Cross-AS map authority:** fault-record-gated (caller == record pager, `fault_id` match); `cap::lookup(CapType::Frame, CAP_RIGHT_WRITE)` in the pager's CSpace (acquires a reference, balanced by `obj->release()` after `map`); NX-only; `count <= fc->count`; PTEs unmap on rollback.
7. **Syscall table integrity:** 69–73 with `MAX_SYSCALL=74`; none of the five pager syscalls in `k_syscall_fast[]` (only YIELD/GET_TICKS/PRINT/CREATE_MAILBOX/DESTROY_MAILBOX/GETPID/PAUSE/REBOOT/HALT); `SYS_PAGER_RECV` validates the `CheckedPtr` destination **before** `recv`, and `recv` itself only copies (record stays pending), so a failed copy loses no fault notification.
8. **Watchdog:** `deadline_tick = ticks + 1000`; per-fault fail-closed (registration KEPT, VA poison latch F9); eviction only on pager death/unregister; `map_in_progress` records deferred (deadline absorbs a skipped scan).
9. **Cleanup/exec ordering (check 7):** `drain_task(client)` before `free_user_pages`; `drain_task(pager)` before `Notify` destruction; `FrameCap::dispose/revoke → invalidate_cap`; `blocked_on_pager_fault` cleared only by `wake_client`, never read for control flow.
10. **Test honesty (check 10):** the roundtrip is non-vacuous — a real ring-3 probe (`userspace/pager-probe.c`) registers PID 1, faults at `0x10000000` (log: `CR2=0x10000000`, err=0x6), the #PF ISR delegates, the harness maps, the probe retries the faulting write and `_exit(0)` (`exit_code == 0` asserted); the watchdog/abort/death/revoke tests all drive real SIGSEGV-terminations. Pager counters zero-delta across all 13 tests.

---

## DECISION

**REJECTED** — one S1 (N1: `drain_task` Work-array stack overflow, 136 > 128) and one S2 (N2: `invalidate_cap` single-entry-per-slot closure defect).

All six iteration-1 findings (F1–F6) are correctly remediated and verified at runtime (13/13 PASS, Pager counters zero-delta; residual `Cap objects +1 / MemPool[7] +1` is the harness CNode, identical to the cap_shm baseline). The S1 rejection is new: the committed-pin remediation itself introduced a stack overflow in `drain_task` (unbounded by one pending record per slot), and the revoke closure removes only one committed entry per slot per call. `audits/rejected_patch.diff` (git apply-able on top of the pending patch, verified with `git apply --check` + byte-identical reproduce) fixes both; re-audit is required.

DECISION: REJECTED