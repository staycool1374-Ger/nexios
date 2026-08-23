# Open Issues

## Kernel — ElfLoader (H2 deferred-switch family)
- [x] **`elf_loader` class flake — RESOLVED (commit `b85ba27d`)**. Root cause (GDB-confirmed, 2026-08-15): the loader's `task_main` idle-block critical section acquired `ElfLoader::lock_` and set self BLOCKED + `dequeue_ready` WITHOUT an IrqGuard. A timer-ISR preemption could land between the lock acquire and release, leaving the loader BLOCKED while still holding `lock_` (`lock_.holder_=task_main`, loader state=BLOCKED). The harness's `reset()`/`request_load` then spun on `lock_` forever; the timer never re-dispatches a BLOCKED loader, so the lock was never released. Fix: wrap the critical section in `arch::IrqGuard` (lock is never held across a preemption; release + `reschedule()` stay outside). Validated (trace OFF): elf_loader 10/10 consecutive clean runs; full debug `all` gate 873/873 ×2 (was hung at test 191).

## Kernel — VM / Page Table
- [x] **pml4_clone test class crashes — RESOLVED (commits `d535a853` + `60d02e49`)**. Root cause (GDB-confirmed 2026-08-15): low-VA `map_page`/`unmap_page` (0x200000/0x400000, pml4_idx < PML4_USER_COUNT) split boot 2 MiB huge entries in the kernel identity PD (phys 0x3000) into pool PT pages; `snapshot_restore` restored only the higher-half PD (0x5000) gated on `hhdm_modified_` (never set by low-VA maps), so PD_IDENTITY dangled to rewound pool pages. Over ~485 cycles the stale entries were zeroed, clearing shared PD_HIGHER entries (incl. PD[5] covering the snapshot buffer at 10 MiB) — the harness's private PML4 (MP-1) faulted reading `g_snapshot` during snapshot_restore (test_isolate.cpp:817; CR3=0x7ff9000, PD_HIGHER[4..6]=0). Secondary: snapshot_create's guard-page map/unmap split PD_HIGHER[5] after the ResourceTracker/pool baseline capture, causing a +1 leak + dangling reference. Fixes: (1) `identity_modified_` flag + PD_IDENTITY save/restore in the snapshot; (2) vmm tests use scratch private PML4s (no kernel identity-PD splitting); (3) ResourceTracker + pool snapshot captured after the guard-page block. Validated: memory_vmm 10/10 ×4 (was leak+panic), debug `all` 873/873 (leaks 16→12; remaining are pre-existing MemPool[8] +1).

## Kernel — Scheduler / IPC (H2 deferred-switch race)
- [x] **`ipc_send_sync_roundtrip` hang — RESOLVED (commits `4bf751b4` + `b85ba27d`)**. Two independent root causes fixed 2026-08-15: (1) `switch_to_task` owner-resolution self-switch no-op (`4bf751b4`): owner-resolution could correct `current` to the physical runner after the entry `current==&next` check, publishing a SELF-switch that iretq'd the runner onto its own stale frame; (2) elf_loader `task_main` lock_ held across a timer-ISR preemption (`b85ba27d`), deadlocking the harness. Validated (trace OFF): ipc_core 20/20 + 5/5 clean; full debug `all` gate 873/873 ×2.

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
