# Open Issues

## Kernel — ElfLoader (H2 deferred-switch family)
- [ ] **`elf_loader` class flake — `wait_loader_idle` ENSURE panic** (`state() == LoadState::IDLE` at elf_loader.cpp:245). Intermittent, ~50% (baseline `2f1a7faf` 2/3 pass, 1 fail; observed 1–4/8 on `main`). Fails at test 2 (`loader_load_invalid_elf`) right after `[SNAP:RESTORE]`+`[RUN_PRE]`: the loader task (ID=5, prio 15) is BLOCKED with non-IDLE state and never re-enters the ready queue (`[RS] cur=1 next=0 hi=0`), so `wait_loader_idle()`'s 1M-spin loop times out → ENSURE panics. Same family as the H2 deferred-switch race (BUGS.md below): the harness/loader task resumes on a stale iret frame after snapshot_restore. Not caused by Phase-2 dmesg/klog encapsulation (baseline reproduces). Needs the same GDB watchpoint investigation (`scheduler_load_rsp_from`/`scheduler_switch_generation`) as H2. Symptom signature identical to `ipc_send_sync_roundtrip` hang (armed deferred switch never applied; no further dispatch).

## Kernel — VM / Page Table
- [ ] **pml4_clone test class crashes** — Page Fault after `pml4_fork_no_child_corrupt_parent` and `pml4_free_user_pages_shared_safe` with CR3 corruption (tests 485-486 in all-1). Triggered during snapshot_restore after PML4 clone/fork operations. CR3=0x1000 suggests freed page table. Blocked by HHDM snapshot restore.

  **2026-08-15 investigation (developer session, commit `d5cc64f3`):**
  - `CR3=0x1000` is the NORMAL kernel PML4 (`boot.asm` PML4_BASE; `vmm.cpp:37` `kernel_pml4_ = read_cr3()`). The crash is a kernel-mode Page Fault with the standard kernel PML4 active — i.e. corrupted KERNEL identity page tables, not a stale user CR3.
  - Root cause identified: `test_vmm.cpp:52/70` `VMM::map_page(0x400000/0x401000, ...)` walks PML4[0]→PDPT[0]→PD[2] of the KERNEL identity map (PD_IDENTITY phys 0x3000, boot.asm:88-99). `get_table` (vmm.cpp:140) splits the 2MB huge entry into a PT page allocated via `PMM::alloc_page_table()` (kernel page-table pool). snapshot_restore restores the HIGHER-half PD (0x5000) gated on `hhdm_modified_` (test_isolate.cpp:674-724) but NEVER restores the LOW identity PD (0x3000) — low-VA map_page does not set `hhdm_modified_` (vmm.cpp:268 requires `pml4_idx >= PML4_USER_COUNT`). The split PT is rewound by the pool snapshot but the identity PD entry still points at the freed page → cumulative corruption after ~485 cycles.
  - Attempted fix (REVERTED): save/restore the identity PD (0x3000) in snapshot_create/restore, mirroring the HHDM PD block. The memcpy-only variant (PT pages reclaimed by the PtPoolSnapshot restore) removed the Double Fault but the standalone `memory_vmm` class still fails from a PRE-EXISTING +1 PMM leak per test (`vmm_map_already_mapped`, `vmm_map_page_null_phys`, `vmm_unmap_already_unmapped`, `vmm_huge_page_split_corner`) + crash — reproducible on baseline WITHOUT the fix. The leak is `alloc_page_table` pool pages tracked via `track_pmm_alloc(1)` (pmm.cpp:441) that `restore_pool_snapshot` (pmm.cpp:841-862) rewinds in the bitmap but does NOT reconcile in ResourceTracker's `pmm_pages_used` counter — likely a false-positive accounting gap. Validation of the identity-PD fix is also blocked by the elf_loader trace-OFF flake (`all` hangs at test 191).
  - Next: (1) reconcile pool-snapshot restore with ResourceTracker pmm counter (fix the +1 leak first, restoring `memory_vmm` to 10/10), (2) re-apply the identity-PD save/restore, (3) validate via `all` once the elf_loader flake is resolved.

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

  **2026-08-12 investigation (developer session, branch `main` @ `464f1fbc` + committed fix `b7dce519`):**
  - Confirmed the WEDGE `[WEDGE] blocked-in-runq` INV-2 violation is a REAL bug independent of the hang: `IPC::block_sender()`/`wake_sender()` PI boost called `move_priority()` unconditionally, re-enqueueing a BLOCKED owner. Fixed + committed `b7dce519` (gate on `owner.in_ready_queue_`, mirroring `set_priority`). `[WEDGE]` count 15→0, all ipc/sync classes still green. The hang persists → WEDGE was NOT the hang's root cause.
  - Hardware-watchpoint session attempted per `tools/gdb/h2_wp2.py` / `h2_replay*.gdb` (updated to current addresses). Confirmed the documented dead end (docs/_archive/investigation-cumulative-corruption.md §"Key dead end"): **QEMU gdb-stub DR0-3 watchpoints never fire** (both lldb `WatchpointCreateByAddress` and gdb `watch` accept but never trigger; breakpoints DO fire). The embedded `[H2W]` kernel recorder in `switch_to_task` is the working instrument but did not fire in failing runs (this freeze is a different variant: harness dispatched onto its OWN stale KSLO frame, not an orphaned page).
  - Added cold-path `[H2-ABORT]` diagnostic to `scheduler_validate_pending_switch` (fires only on stale-RSP arm drop): in the failing run the arm to receiver 6 PASSED validation (no `[H2-ABORT]`), so the abort path is NOT the mechanism.
  - Added cold-path `[H2-DISP]` (harness dispatch frame audit, CONFIG_DEBUG_IPC_SCHED): the harness is ALWAYS dispatched onto its KSLO-window kstack frame (`context.rsp` = `0xFFFF9000000329xx`, `kernel_stack` = `[0xFFFF900000023000-0xFFFF900000033000]`), NOT the linker boot stack — this is the documented H2 displacement and is NORMAL (43/43). Direction 1 in `restore_task_fields` correctly preserves the LIVE kslot RSP (`same=1`, `live_rsp=0xFFFF900000032998`).
  - **Root-cause candidate (new evidence):** the harness's stored iret frame content at its kslot `context.rsp` is occasionally STALE — a leftover frame from a PREVIOUS test (failing run shows `rip=test_ipc_wake_sender_restores_priority`@`arch::sti()`, i.e. test-19 code; passing runs show `rip=arch_hlt`, i.e. the harness's own wait-loop hlt). When the deferred switch iretq's the harness onto that stale frame, it resumes inside completed test code (not the wait loop), eventually re-arming/deadlocking with no further ticks. Direction 1 fixes the RSP *address* but not the *frame contents* at that address. The exact write that leaves stale test-19 code at the harness's kslot `context.rsp` remains to be pinned.
  - **Next step (not yet done):** instrument `restore_task_fields`/`switch_to_task` save path to capture WHO writes the harness kslot frame's `rip` to a test-function address between tests (likely the snapshot-restore `context = saved[j].context` deep copy restoring a snapshot-time frame, or a test task's stack reuse of the same kslot slot). Candidate fix: after Direction 1 re-applies the live RSP, rebuild a fresh valid iret frame (rip=`arch_hlt`, IF=1) at that address so the harness always resumes at its wait-loop hlt.


# Resolved

### #021 — all-1 GPF at IpcConcurrentSenders (test 80/745)
- **Fix:** `PMM::is_allocated()` safety check in `VMM::get_table()` — before following a PAGE_PRESENT entry, verify the target page is allocated. If not, clear the entry and fall through. Tested: `IpcConcurrentSenders` passes, all-2 passes 133/133, all-1 reaches test 485 without GPF.
- **Committed:** `210feb06` (stale-entry guard + HHDM limit + clear_pte_in_pml4)

### #022 — PCP mutex retry budget exhaustion
- **Root Cause:** Direct ownership transfer in unlock() was missing for the original wake_one()+restore_priority pattern. The lock-stealing race caused PCP retry budget exhaustion.
- **Fix:** Direct ownership transfer in `unlock()`/`unlock_err()` prevents lock stealing. `restore_priority()` ordering fixed (move after waiter removal). 6 test classes migrated to `lock_err()`.
- **Committed:** `52d19137`, `afdf3b84`, `8defb9af`
