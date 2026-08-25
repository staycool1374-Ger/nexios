# STATE — Current Work State

> Moved out of AGENTS.md (session state, not operating rules).
> Update after each milestone; AGENTS.md only links here.

## Active Summary
### Objective
- **DONE (2026-08-25):** Issue #5 "MP-4.4 — aarch64 PAN/PXN enablement" — real aarch64 PAN enablement + PXN/UXN closure + 5 new arch_aarch64 tests, SIL 3 APPROVED (audits/report-2026-08-25T15-05-22Z.md), issue CLOSED. Commit `1577242c`.
- **DONE (2026-08-25):** Issue #6 "Implement all pending audits/refactorings under audits/" — all 8 kernel remediation phases (P0–P8) implemented, SIL 3 APPROVED per phase, issue CLOSED. Full debug `all` **932/932 PASSED** (trace OFF), release selftest 85/85. P9 (test-hygiene backlog ~150 purges) deferred to testbed.
- **DONE (2026-08-25):** Multi-arch compile-clean (#99, commit `36fa5118`): `make debug NO_LTO=1 ARCH={aarch64,riscv64}` now link green to a kernel ELF (QEMU virt targets) without changing x86_64 runtime behavior. SIL 3 approved (`audits/report-20260825T060240Z.md`).
- Previous: `make build` green (check-style error gate) after the scheduler-deadlock fix and the full style/clang-tidy cleanup.

### Important Details
- Branch `main`. Issue #5 commits: `1577242c` (feat aarch64 PAN/PXN), `0b1bd6e9` (test-history rows).
- **Issue #5 MP-4.4 (aarch64 PAN/PXN):** SCTLR_EL1.PAN (bit 23) gated on ID_AA64MMFR1_EL1.PAN[23:20]!=0 in `arch::pan_init()`; CONFIG_PAN; real stac/clac/read_rflags via PAN sysreg s3_0_c4_c2_4 (PSTATE.PAN bit 22), normalized bit-18 AC-equivalent contract, degraded no-op when unsupported. PXN(53)/UXN(54) closure: attr_from_flags USER PXN (SMEP parity), VMM aarch64 map_page/map_page_in_pml4 + block-split base_flags. Latent L3 DESC_TABLE (0b11) leaf fix. 5 new arch_aarch64 tests (arch_aarch64 17→22). `aarch64_page_table_map_leaf` TEST_VA moved off a boot 2MB block.
- **aarch64 runtime reality (2026-08-25):** kernel BOOTS in QEMU (cortex-a72) through arch init but PANICs at meminit — PMM OOM (pmm.cpp:357/mempool.cpp:55), DTB memory regions never seeded into PMM. Filed as issue **#100** under #28 scope. QEMU cortex-a72 reports PAN==0 → PAN enable path is runtime-gated, tests exercise degraded mode until a real PAN-capable boot exists.
- Issue #6 remediation commits: `eb3796e8` (P1 sync), `09c7490e` (P2 UAF), `02c799bd` (P3 TCB), `27bd582b` (P4 sched ID), `9e32d65a` (P5 IPC/cap), `b39b0066` (P6 drivers/net), `0d3c02d4` (P7 LSTAR), `9a558d7a` (P8 sync mediums).
- **Issue #6 audit backlog (9 docs) remediated:**
  - audit-task-sync-v0.4.2: C-1/C-2/H-1/H-6/C-3/M-8 (sync blocking paths); H-2/H-3/H-7 (notify/queue gen-tags + PIP re-bucket + locked BLOCKED); H-5 (TCB::destroy); H-4/M-2/M-5/M-9 (sync mediums).
  - audit-scheduler-tasks + scheduler_audit: H-1..H-4 (alloc_id contract, atomic next_task_id_, %u fixes), M-1/M-2 + LOW (validate_iret_frame, task_stack_ptr inline, IRET offsets).
  - audit-ipc-cap-syscalls: H-2/H-3/H-4 (BufferPool owner/VA/TLB, Endpoint dispose drain), M-2/M-4/M-5/M-6.
  - audit-drivers-vfs-net: H-1/H-2/H-3/M-1..M-5 (AHCI/virtio serialization, GHC_IE removal, net globals lock, poll bounds).
  - gs-base-swapgs: LSTAR/sysret landmine removed (int $0x80 sole live path), snapshot IF=0 ENSURE.
  - deep-analysis-h2 / FLAW-01/02/03/08: verified already-resolved (P0).
- **Multi-arch portability (from #99):** parse-time arch-stamp clean; arch-keyed cpp-rules; gen_test_registry --arch; CONFIG_ARCH_X86_64 guards.
- **Key invariant now enforced tree-wide:** no spinlock held across Scheduler::reschedule(); BLOCKED tasks dequeued from the ready queue (INV-2); generation-tagged waiter/peer pointers; ENSURE only for impossible invariants.
- **Known caveats:** aarch64/riscv64 COMPILE-CLEAN only (issues #28/#29). M-1 per-spin budgets, M-3 trace gating, queue/semaphore init flags deferred (documented in code). syscall_entry.asm retained as commented dead file (deletion needs user approval).
- **Pre-existing H2 deferred-switch race** (BUGS.md, RE-OPENED): ~50% flaky TIMEOUT/PANIC in ipc_core/elf_loader/all/selftest; reproducible on baseline. (Issue #6's full `all` run passed 932/932 trace OFF, but the flake is not eliminated.)

### Work State
#### Completed
- **ISSUE #5 MP-4.4 — aarch64 PAN/PXN enablement (2026-08-25, commits `1577242c`+`0b1bd6e9`, SIL 3 approved):** SCTLR_EL1.PAN (bit 23) + ID_AA64MMFR1_EL1.PAN detection (arch::pan_init, early_init.cpp); CONFIG_PAN; real PAN-sysreg stac/clac/read_rflags (normalized bit-18 contract, degraded no-op); PXN/UXN closure in attr_from_flags + VMM aarch64 paths; latent L3 DESC_TABLE leaf fix; 5 new arch_aarch64 tests; map_leaf TEST_VA moved off boot 2MB block; vmm.hpp PXN comment fix; riscv64 comment pointer. Gates: aarch64 compile+link green, x86_64 arch_cross 21/21. aarch64 runtime test execution deferred to #28 (boot OOM = #100).
- **GLOBAL-VARIABLE ENCAPSULATION REFACTOR (2026-08-14, commits `8334a120`..`f37b3e2d`, SIL 3 approved):**
  - **Phase 1** (`1b569a8c`): dead code + linkage hygiene — removed `g_boot_ns` (zero readers); made `g_virtio_net_dev`, `g_h2_ring`/`g_h2_idx`, `fat32_root_vnode`, `g_watchdog_*` static/TU-local; deleted stale `scheduler.cpp.bak*`.
  - **Phase 2** (`a99bf374`): `g_dmesg` → `log::DmesgService`, `g_klog` → `log::KlogService` (Meyers singletons, private ctor + buffer); `dmesg_push*` macros → inline fns; all consumers migrated.
  - **Phase 3** (`75a47590`): `src/kernel/core/global_state.{hpp,cpp}` created — single definition point; `verify_and_write<T>` (IDEMPOTENT/BOOT_ONLY/NEVER_WRITE rules + CONFIG_DEBUG audit ring); BootState/FaultState/TestState accessors; duplicate defs + unused externs removed (kernel.hpp/test.hpp/syscall.hpp/test_isolate.hpp).
  - **Phase 4** (`43dd2f2d`): VfsState — `fat32_partition_instance` → `try_set_fat32_partition` (RANGE_CHECKED: null or kernel-half). Filesystem singletons stay module-owned (documented).
  - **Phase 5** (`34d7cf2d`): NetState — `g_nic` → `try_set_nic` (RANGE_CHECKED). Daemon PIDs left file-static (already accessor-wrapped).
  - **Phase 6** (`e06790ec`): AsmSwitchState — 14 deferred-switch globals (scheduler_save_rsp_to, isr_nesting_depth, fpu_owner, …) moved to global_state.cpp `extern "C"` block, byte-identical symbols for isr_stubs.asm; `kernel::fpu_owner` qualifier fixes.
  - **Phase 7** (`f37b3e2d`): docs + full gates — **debug `all` 870/870 (trace ON), release `all` 84/84 (trace OFF)**.
- Scheduler deadlocks fixed + committed (`dfc3aec`): pop_front cycle-guard, rebuild_ready_queue flag-clear, ready_queue_manager in_ready_queue_ maintenance + restore_pod, wake_waiting_parent, reap test, TEMP DEBUG removed. 16 runs clean.
- Style errors 128 → 0: added `#pragma once` to 105 headers; fixed 117 `init_required` value-initializers; fixed 2 `no_const_cast` (block_device.hpp/.cpp param type, virtio_blk.cpp staging buffer); added `arch::pause()` to 5 infinite loops.
- Checker false-positive fixes (tools/validate_style.py): skip assembly (`;`, .S/.asm); placement-new; `break`/`return` in `while(true)`; `wfi` halt loops.
- clang-format applied to 297 files via new `.clang-format` (ColumnLimit 80, Attach braces, 4-space, SortIncludes false, BreakStringLiterals false); build verified clean.
- Cheap-high-value path DONE: fixed 2 real `branch-clone` (scheduler.cpp can_reap ~698-707, fat32_fs.cpp SEEK_END/default); disabled 3 clang-tidy checks in Makefile (performance-no-int-to-ptr, bugprone-reserved-identifier, bugprone-easily-swappable-parameters).
- MemoryChecker in tools/validate_style.py REWRITTEN to a brace-depth-aware function-nesting stack (func_stack + pending_func) with: boot-alloc exemption set (_boot_alloc_funcs: AhciDriver::probe, AtaPioDriver::probe_first_drive, VirtioBlkDriver::probe, virtio_net_probe, higherhalf_entry); control-keyword exclusion (_ctrl_keywords); skip of `#`/comment lines; and `\b` word boundaries on `_new_delete` (r"\bnew\b\s|\bdelete\b\s|\bmalloc\s*\(|\bfree\s*\(") to stop false matches on `is_free(`/`bufpool_free(`/`track_*_free(`.
- VERIFIED: `make check-style` → Errors: 0, Passed. `make debug NO_LTO=1` → ISO built cleanly.

#### Active
- (none outstanding) — issue #5 closed; issue #6 fully remediated (P0–P8, SIL 3 approved, closed); full `all` 932/932.

#### Blocked
- **aarch64 runtime boot (#28/#100):** kernel boots to meminit then PMM OOM panic (DTB memory regions never seeded into PMM) — blocks all aarch64 runtime tests incl. the 22-test arch_aarch64 class. P9 test-hygiene (testbed) is future work.

### Next Move
- Optional: reduce the non-blocking style warnings and clang-tidy warnings.
- **aarch64 runtime boot (issues #28/#100):** seed PMM from DTB memory regions (fix the meminit OOM), then run the arch_aarch64 class (22 tests) + PAN runtime enablement on a PAN-capable CPU model.
- **P9 test-hygiene (issue #6 remainder):** on `testbed` — ~50 Rule-4 `remove_task+cleanup+delete` sites → `terminate_and_drain`, ~150 stub/tautology purges (§2.5 of audits/test-suite-v0.3.10.md).
- Next multi-arch step (when scheduled): riscv64 runtime boot path (issue #29).
- Tracked separately: the pre-existing H2 deferred-switch race (BUGS.md, RE-OPENED) — ~50% flaky TIMEOUT/PANIC, reproducible on baseline.

### Relevant Files
- src/kernel/arch/aarch64/hal/io_impl.hpp: real stac/clac/read_rflags (PAN sysreg s3_0_c4_c2_4, PSTATE.PAN bit 22, normalized bit-18 contract) + g_pan_supported/pan_init decls.
- src/kernel/arch/aarch64/early_init.cpp: arch::pan_init() — SCTLR_EL1.PAN (bit 23) gated on ID_AA64MMFR1_EL1.PAN[23:20], g_pan_supported definition.
- src/kernel/arch/aarch64/hal/page_table_impl.hpp: attr_from_flags USER PXN (SMEP parity), L3 DESC_TABLE (0b11) leaf fix.
- src/kernel/memory/vmm.{hpp,cpp}: PAGE_UXN/PAGE_PXN constants; aarch64 map_page/map_page_in_pml4 + block-split base_flags UXN|PXN propagation; PXN comment fix.
- src/kernel/kernel.cpp: pan_init() + "[BOOT] PAN ..." log (aarch64, CONFIG_PAN).
- src/kernel/nexios_config.h: CONFIG_PAN (default 1 on aarch64).
- src/kernel/arch/aarch64/test_aarch64.cpp: 22 tests (5 new PAN/PXN walk/register-level); walk_leaf_descriptor helper.
- src/kernel/test/test_expected_counts.hpp: arch_aarch64 aarch64 col 22.
- src/kernel/sync/{semaphore,eventgroup,queue,notify,mutex}.cpp: lock-scope-before-reschedule + dequeue_ready (INV-2) + generation-tagged waiters + PIP move_priority re-bucketing + H-4 retry.
- src/kernel/task/{task,scheduler}.cpp: TCB::destroy() pool-aware helper; alloc_id table-full contract; atomic next_task_id_; validate_iret_frame + task_stack_ptr() inline.
- src/kernel/ipc/{buffer_pool,ipc}.cpp + src/kernel/cap/endpoint.cpp: map() owner/VA/TLB-flush; dispose drains senders + disposed_ re-check; self-send guard; PI-undo in ~MessageQueue.
- src/kernel/driver/{ahci,virtio_blk,virtio_net}.cpp: per-port/device mutexes, GHC_IE removal, used-idx pre-notify snapshot, desc_idx bounds, destroy drains.
- src/kernel/net/net.cpp: module SpinLock + atomic g_ip_ident.
- src/kernel/syscall/syscall.cpp: LSTAR/STAR/FMASK removal (int $0x80 sole path); syscall_entry.asm dead (unassembled in mk/rules.mk).
- src/kernel/task/task_queue.cpp: orphan-drop pop_front; src/kernel/sync/spsc_ring.hpp: consistent atomics.
- docs/specs/drivers.md: FLAW-06 ledger BOUNDED.
- Makefile/mk/rules.mk/tools/gen_test_registry.py: multi-arch portability (from #99).
- src/kernel/core/global_state.{hpp,cpp}: single definition point for all cross-TU kernel globals (7-phase refactor).
- BUGS.md: elf_loader H2-family flake logged (2026-08-14).
