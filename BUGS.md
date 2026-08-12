# Open Issues

## Kernel — VM / Page Table
- [ ] **pml4_clone test class crashes** — Page Fault after `pml4_fork_no_child_corrupt_parent` and `pml4_free_user_pages_shared_safe` with CR3 corruption (tests 485-486 in all-1). Triggered during snapshot_restore after PML4 clone/fork operations. CR3=0x1000 suggests freed page table. Blocked by HHDM snapshot restore.

## Kernel — Scheduler / IPC (H2 deferred-switch race, RE-OPENED)
- [ ] **`ipc_send_sync_roundtrip` hang — H2 deferred-switch race NOT resolved** (was marked resolved in ROADMAP_done 2026-08-08 / AGENTS.md; re-verified broken 2026-08-12 on branch `testbed` @ `a2750bd2`).

  **Reproduce:**
  1. `git checkout testbed` (HEAD `a2750bd2`, v0.4.0 merged)
  2. `make execute-test x86_64 debug ipc_core` (or legacy `ipc` class)
  3. Repeat until hang — measured rate ~50% (2/4, 4/8 for `ipc_core`; 1/6 for legacy `ipc` class). Deterministic within the run: passes tests 1–20 (`ipc_send_block_full` is 20/23 PASS), then wedges at test 21 `ipc_send_sync_roundtrip`.
  4. What fails: harness never reaches `TEST SUMMARY`; host-side watchdog kills QEMU after 120s (`RESULT: TIMEOUT (no TEST SUMMARY within 120 s)`).

  **Serial-trace evidence (with `CONFIG_DEBUG_IPC_SCHED` enabled):**
  ```
  [SW] cur=7 next=1 rsp=...          (task 7 -> harness, applied)
  [APPLY] id=1 cur=1
  [SW] cur=1 next=6 rsp=...          (harness -> receiver task 6, ARMED)
  <no [APPLY] id=6, no further [TICK]>   <- deferred switch never applied; ticks stop
  ```

  **Key observations:**
  - The deferred switch to the receiver (task 6, prio 11) is armed but never applied; no further timer ticks are emitted → the harness's `hlt()` loop waits forever.
  - `CONFIG_DEBUG_IPC_SCHED` **no longer masks the race**: trace ON still hangs 3/4 (contradicts AGENTS.md "881/881 with trace ON" / ROADMAP_done "resolved" claims).
  - Not caused by the test-class re-organization: reproduced on the legacy `ipc` class with the re-org stashed.
  - H2 fix commits (`8d06a947`, `56549b7b`, `f7b2278a`) are ancestors of HEAD; the six/four guard layers are present in `scheduler.cpp`, yet the race still triggers ~50%.
  - Suspect: interaction with v0.4.0 MP-1 private kernel-half PML4 / CR3-publish per-switch overhead (per-switch CR3 publishes + canary verify shift dispatch cadence), or a remaining arm/apply asymmetry under the new dispatch path. Needs GDB watchpoint session on `scheduler_load_rsp_from`/`scheduler_switch_generation` (per AGENTS.md Debugging Protocol steps 3–4).


# Resolved

### #021 — all-1 GPF at IpcConcurrentSenders (test 80/745)
- **Fix:** `PMM::is_allocated()` safety check in `VMM::get_table()` — before following a PAGE_PRESENT entry, verify the target page is allocated. If not, clear the entry and fall through. Tested: `IpcConcurrentSenders` passes, all-2 passes 133/133, all-1 reaches test 485 without GPF.
- **Committed:** `210feb06` (stale-entry guard + HHDM limit + clear_pte_in_pml4)

### #022 — PCP mutex retry budget exhaustion
- **Root Cause:** Direct ownership transfer in unlock() was missing for the original wake_one()+restore_priority pattern. The lock-stealing race caused PCP retry budget exhaustion.
- **Fix:** Direct ownership transfer in `unlock()`/`unlock_err()` prevents lock stealing. `restore_priority()` ordering fixed (move after waiter removal). 6 test classes migrated to `lock_err()`.
- **Committed:** `52d19137`, `afdf3b84`, `8defb9af`
