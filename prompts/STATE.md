# STATE — Current Work State

> Moved out of AGENTS.md (session state, not operating rules).
> Update after each milestone; AGENTS.md only links here.

## Active Summary
### Objective
- **IN PROGRESS (2026-08-31):** Issue #100 remainder COMPLETED — aarch64 daemon-ready handshake + full arch_aarch64 bring-up. Root causes: (1) aarch64 syscall ABI dispatched on x0 not x8; (2) switch_away_from_terminating used x86-only iret-frame validation (discarded dequeued init → stranded READY); (3) aarch64 kernel-task SPSR=0x3C5 masked IRQs → voluntary reschedule never applied → harness starved; (4) page-table phys derefs needed HHDM + 48-bit phys mask; (5) PCI ECAM base wrong (0x100000000 → 0x30000000); (6) boot page tables at 0x40000000 clobbered DTB region (relocated to 0x40020000); (7) RTC used cntpct not PL031. Gates: aarch64 arch_aarch64 **22/22**, x86_64 debug all **978/978**, x86_64 release all **85/85**. SIL 3 audit pending.
- **DONE (2026-08-30):** Issue #100 primary defect + 8 first-run defects fixed (commits `adde2324`+`e24b1415`), SIL 3 APPROVED; arch_aarch64 TIMEOUT at daemon-ready handshake was the remainder.
- **DONE (2026-08-30):** Issue #4 "IOMMU DMA protection (VT-d / AMD-Vi / SMMU)" — capability-gated identity-IOVA VT-d table infrastructure (IoMmuDmaCap + IoMmuManager + SYS_IOMMU_MAP(59)/SYS_IOMMU_UNMAP(60)); live-DMAR programming documented as phase-2. SIL 3 APPROVED (audits/report-2026-08-30T11-14-39Z.md, 3 iterations). Class cap_iommu 12; debug `all` registered 990 (975 executed, 0 fail); release all 85/85; selftest 133/133. Commit `bf0984b1` (closes #4).
- **DONE (2026-08-30):** Issue #2 "IRQ caps + user-space IRQ delivery (IrqCap)" — IrqCap (CapType::Irq) + bounded owner-tagged IRQ delivery table (CONFIG_CAP_MAX_IRQ=16) + SYS_IRQ_REGISTER(57)/SYS_IRQ_WAIT(58). SIL 3 APPROVED (audits/report-2026-08-30T00-08-05Z.md, 3 iterations). Class cap_irq 12; debug `all` registered 978 (963 executed, 0 fail); release all 85/85. Commit `ef9b748b` (closes #2).
- **DONE (2026-08-29):** Issue #1 "Untyped child-split + sub-range carve" — cap::retype exhaustion-model sub-range carve (PAGE-aligned, prefix-only) + child Untyped for the remainder + SYS_CAP_RETYPE (56). SIL 3 APPROVED (audits/report-20260829T181710Z.md, 3 iterations). Class cap_untyped 9→18; debug `all` registered 966 (951 executed, 0 fail); release all 85/85; selftest 133/133. Commit `3eb2a50b` (closes #1).
- **DONE (2026-08-25):** Issue #3 "MMIO caps + fine-grained I/O delegation" — MmioCap (CapType::Mmio) + VMM::map_mmio_from_cap + SYS_IOPORT_GRANT per-task TSS I/O bitmap delegation; SIL 3 APPROVED (2 reports), issue CLOSED. Commits `08c21a5f`+`adaef25a`+`4d1bca31`.
- **DONE (2026-08-25):** Issue #5 "MP-4.4 — aarch64 PAN/PXN enablement" — real aarch64 PAN enablement + PXN/UXN closure + 5 new arch_aarch64 tests, SIL 3 APPROVED (audits/report-2026-08-25T15-05-22Z.md), issue CLOSED. Commit `1577242c`.
- **DONE (2026-08-25):** Issue #6 "Implement all pending audits/refactorings under audits/" — all 8 kernel remediation phases (P0–P8) implemented, SIL 3 APPROVED per phase, issue CLOSED. Full debug `all` **932/932 PASSED** (trace OFF), release selftest 85/85. P9 (test-hygiene backlog ~150 purges) deferred to testbed.
- **DONE (2026-08-25):** Multi-arch compile-clean (#99, commit `36fa5118`): `make debug NO_LTO=1 ARCH={aarch64,riscv64}` now link green to a kernel ELF (QEMU virt targets) without changing x86_64 runtime behavior. SIL 3 approved (`audits/report-20260825T060240Z.md`).
- Previous: `make build` green (check-style error gate) after the scheduler-deadlock fix and the full style/clang-tidy cleanup.

### Important Details
- Branch `main`. Issue #4 commit: `bf0984b1` (feat IOMMU DMA protection).
- **Issue #4 (IOMMU DMA protection):** `CapType::IoMmuDma=8`; `IoMmuDmaCap : KernelObject` (domain_idx_, owner_task_id_, CONFIG_CAP_MAX_IOMMU, kernel-internal create — probe gate → domain_create → MemPool, rollback on alloc failure); `IoMmuManager` (src/kernel/iommu/) — static bounded domain/root/context tables (CONFIG_IOMMU_MAX_DOMAINS=8/MAPPINGS=32/BUSES=8), identity-IOVA 4-level SL walk (zeroed PMM pages mandatory), unwind-on-failure linked-page stack, cascade-empty-free, overlap/revoked/unaligned rejection, bdf_valid guards, one leaf SpinLock; `SYS_IOMMU_MAP`(59)/`SYS_IOMMU_UNMAP`(60) (MAX_SYSCALL=61) — WRITE = arming authority, strict owner-task check, SL flags from the frame SLOT's rights, absent IOMMU → graceful -1; revoke/dispose destroy the domain FIRST (fail-closed). vtd.hpp = VT-d entry layouts (SL R/W/E bits 0-2, addr 51:12; root P+CTP; CTE P/FPD/T=1/ASR 63:12). Phase-2 (live DMAR/GCMD/IOTLB/faults, AMD-Vi/SMMU backends) sketched in docs/specs/iommu.md §8. Class cap_iommu (12 tests); `all` registered 990.
- **Audit gotchas (3 iterations):** (1) a walk that links freshly allocated pages must UNWIND them on later alloc failure (orphaned empty pages = permanent PMM leak the rollback can't reach); (2) raw uint8_t BDF fields × fixed encoding = OOB (2295 vs 256 entries) — validate device<32/function<8 at every indexing site.
- **Test-environment gotchas (full `all` gate):** tests running under `all` share the environment with boot-time system tasks — the prio-15 elf-loader legitimately outranks harness tasks, so `next_task()` assertions must be invariants (`>= enqueued prio`), not exclusive equality; hard-coded resource budgets rot (idle CRC test's 200-iteration bound = 800 KiB text budget vs 831,544 runtime bytes) — derive bounds from runtime link symbols (kernel::integrity::_text_start/_text_end) + slack. `git add -N` markers are dropped by stash pop — always verify `grep -c '^diff --git'` matches the expected file count in audits/pending_patch.diff before auditing.
- **Issue #2 (IRQ caps + user-space IRQ delivery):** `IrqCap : KernelObject` (vector, reg_idx_, CONFIG_CAP_MAX_IRQ, single-owner per vector, timer vector 32 reserved, kernel-internal create); `IrqDelivery` static bounded owner-tagged table (`IrqRegistration.owner` = claiming cap) — `arm(reg_idx, owner, recipient)` and `release_slot_idx(reg_idx, owner)` revalidate ownership+vector under the slot lock in ONE atomic critical section; `claim_slot()` IrqGuard-serialized; `isr_entry` gated on `armed && vector` (fall-through to generic handler, single tail EOI); `SYS_IRQ_REGISTER`(57)/`SYS_IRQ_WAIT`(58) via current_cspace (register=WRITE, wait=READ); wait mirrors Semaphore::wait (register waiter once → dequeue_ready+reschedule → spin on BLOCKED — NO sti/hlt/cli for kernel tasks); revoke/dispose/drain wake blocked waiter with -1 (never BLOCKED forever); `TaskControlBlock::cleanup()` drains the table; PIC line-mask state captured at arm / restored at release. Class cap_irq (12 tests); `all` registered 978. **Audit gotcha (3 iterations):** capability slots shared via grant survive owner death — a stale stored reg_idx_ must never arm/release a drained-and-reused slot; check+act must be one lock scope.
- **Issue #3 (MMIO caps + I/O delegation):** `MmioCap : KernelObject` (phys/size/bar_type, CONFIG_CAP_MAX_MMIO, kernel-internal create); `VMM::map_mmio_from_cap`/`unmap_mmio_from_cap` (refuse revoked/IO caps); `SYS_IOPORT_GRANT` (55) — capability-driven per-task TSS I/O bitmap delegation (x86_64): `TSSBlock` (TSS + 8KiB bitmap + 0xFF terminator, iopb_offset=104, default-deny all-1s), `arch::iopb_*` static pool (CONFIG_IOPB_MAX_TASKS=4), owner-memoized bitmap swap on user-task switch, release in cleanup(), snapshot reset. **Found+fixed+audited during gate:** TCB `create/create_user/clone` + ELF loader memset the whole TCB without running the ctor → `iopb_slot_`=0 instead of NONE → every user task loaded the unclaimed all-zeros pool slot into the TSS bitmap (default-deny violated = privilege escalation). Fixed at 4 memset sites + claimed-slot defense (loaded TSS bitmap is always exactly a claimed slot; iopb_claim self-heals). Class cap_mmio (10 tests); `all` registered 957.
- **TCB field sentinel gotcha (recurring):** TCB is memset-zeroed, NOT ctor-initialized — any new field with a non-zero default must be set explicitly after every memset site (create/create_user/clone/elf.cpp), and cross-checked against runtime dispatch behavior.
- Issue #5 commits: `1577242c` (feat aarch64 PAN/PXN), `0b1bd6e9` (test-history rows).
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
- **H2 deferred-switch race — RESOLVED** (2026-08-13/15, commits `71b3a088`, `4bf751b4`, `b85ba27d`; see BUGS.md): stale-resume orphan re-enqueue, owner-resolution self-switch, elf_loader lock-across-preemption. Debug `all` passes with the trace OFF (873/873 ×2, 932/932, 942/942).

### Work State
#### Completed
- **ISSUE #2 — IRQ caps + user-space IRQ delivery (2026-08-30, commit `ef9b748b` "closes #2", SIL 3 APPROVED ×3 iterations):** IrqCap (CapType::Irq, CONFIG_CAP_MAX_IRQ=16, single-owner per vector, timer 32 reserved) + owner-tagged static delivery table (IrqDelivery) + SYS_IRQ_REGISTER(57)/SYS_IRQ_WAIT(58) (MAX_SYSCALL=59). Auditor-driven hardening: owner+vector revalidated under slot lock in arm()/release_slot_idx() as ONE atomic critical section (check-then-act TOCTOU rejected iter-2), claim_slot() IrqGuard-serialized, isr_entry gated on armed+vector (fall-through, single EOI), wait path revalidates occupied/vector/owner/recipient in both lock scopes and clears waiter only when r->waiter==t. Gates: cap_irq 12/12; regressions cap_core 10, cap_untyped 18, cap_mmio 10, cap_syscall 8, cap_lifecycle 8, cap_ipc 6; debug all 963/963 (registered 978); release all 85/85. Evidence: audits/report-2026-08-30T00-08-05Z.md.
- **ISSUE #1 — Untyped child-split + sub-range carve (2026-08-29, commit `3eb2a50b` "closes #1", SIL 3 APPROVED ×3 iterations):** cap::retype sub-range carve (exhaustion model: parent spent, target owns [0,carve), child Untyped owns [carve,size) and is itself retypable); create_subrange() factory (no PMM alloc, shared CONFIG_CAP_MAX_UNTYPED bound counting children); stretch fail-closed on child-creation failure; non-destructive slot-capacity pre-check; SYS_CAP_RETYPE=56 (MAX_SYSCALL=57) via current_cspace(); child installed with rights|CAP_RIGHT_WRITE (no new authority — caller held WRITE over parent). Gates: cap_untyped 18/18; regressions cap_core 10, cap_lifecycle 8, cap_syscall 8, cap_mmio 10, cap_ipc 6; debug all 951/951 (registered 966); release all 85/85; selftest 133/133. Evidence: audits/report-20260829T181710Z.md.
- **ISSUE #3 — MMIO caps + fine-grained I/O delegation (2026-08-25, commits `08c21a5f`+`adaef25a`+`4d1bca31`, SIL 3 APPROVED ×2):** MmioCap (phys/size/bar_type, CONFIG_CAP_MAX_MMIO), VMM::map_mmio_from_cap/unmap_mmio_from_cap, SYS_IOPORT_GRANT (55) + per-task TSS I/O bitmap (TSSBlock + arch::iopb_* static pool + owner-memoized switch + cleanup release + snapshot reset). Found+fixed a real privilege-escalation defect (TCB memset left iopb_slot_=0 → permissive TSS) + claimed-slot defense. Gates: cap_mmio 10/10, cap regressions (10/8/8/6/9), scheduler/hal/vmm regressions, selftest 133/133, debug all 942/942 executed (registered 957), release all 85/85. all-count 941→957. Revocation-limitation documented in cspace.md §2.6 (follow-up).
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
- (none outstanding) — issues #1, #2, #3, #4, #5 closed; issue #6 fully remediated (P0–P8, SIL 3 approved, closed); full `all` 975/975 (registered 990); issue #100 remainder (arch_aarch64 daemon handshake + bring-up) COMPLETED 2026-08-31 (22/22), audit pending.

#### Blocked
- (none) — aarch64 arch_aarch64 class now 22/22; P9 test-hygiene (testbed) is future work.

#### aarch64 bring-up fixes landed (issue #100, 2026-08-31 — uncommitted, SIL 3 audit pending)
- Boot page tables relocated 0x40000000→0x40020000 (was clobbering the QEMU DTB region).
- Syscall ABI: syscall_entry.S dispatches on saved x8 (user x8=syscall number, x0-x3=args), not x0 — every aarch64 user syscall was misrouted.
- Scheduler: switch_away_from_terminating x86-only iret-frame validation → aarch64 ELR/SPSR branch (dequeued init was discarded → stranded READY, never dispatched).
- Kernel-task SPSR: 0x3C5 (all-masked) → 0x345 (I=0) so voluntary reschedule()s are applied by the timer tick (aarch64 kernel tasks were un-preemptible → block loops spun forever).
- Page tables: HHDM alias for all phys derefs + DESC_PHYS_MASK (48-bit; UXN/PXN at bits 53/54 leaked into ~0xFFF phys extraction) + empty-table reclaim in unmap_page.
- PCI ECAM base: 0x100000000 → 0x30000000 (QEMU virt) in hal/pci.hpp + aarch64/pci_impl.hpp.
- RTC: cntpct-based → PL031 wall clock (0x09010000).
- PAN: pan_init clears SCTLR.PAN when FEAT_PAN unsupported (reset value had it set).
- context.hpp init_stack: stack_top by reference (was by value).
- GIC test: verify via ISENABLER (ICENABLER read-back unreliable on QEMU GICv2); DTB test: HHDM deref + tolerate absent DTB (QEMU non-Linux `-kernel` provides none), fallback memory region recorded in boot_info.

### Next Move
- Optional: reduce the non-blocking style warnings and clang-tidy warnings.
- **IOMMU phase-2 (follow-up to #4):** live VT-d — ACPI/DMAR discovery, GCMD/IRTA/RTA programming, IOTLB flush, fault-event handling, kernel-DMA domain, QEMU `-device intel-iommu` integration; then AMD-Vi/SMMU backends behind the same IoMmuManager interface (docs/specs/iommu.md §8). Also: reclaim the per-bus ctx-table slot in clear_attachment + bidirectional static_assert (S3 notes).
- **aarch64 PAN runtime:** arch_aarch64 class green 22/22 on cortex-a72 (PAN unsupported — degraded mode). PAN-capable CPU model (e.g. `-cpu max`/neoverse) would enable the real PAN path.
- **DTB handoff (follow-up):** QEMU `-kernel` non-Linux boots pass no DTB in x0 (no ARM64 Linux header). Optional future work: add the ARM64 boot header so memory comes from the real DTB instead of the platform fallback.
- **P9 test-hygiene (issue #6 remainder):** on `testbed` — ~50 Rule-4 `remove_task+cleanup+delete` sites → `terminate_and_drain`, ~150 stub/tautology purges (§2.5 of audits/test-suite-v0.3.10.md).
- Next multi-arch step (when scheduled): riscv64 runtime boot path (issue #29).
- H2 deferred-switch race: RESOLVED — no longer tracked (see above, BUGS.md).

### Relevant Files
- src/kernel/cap/irq.{hpp,cpp}: IrqCap (vector, reg_idx_, CONFIG_CAP_MAX_IRQ, single-owner create, set_slot_owner).
- src/kernel/irq_delivery.{hpp,cpp}: owner-tagged static delivery table — atomic arm(owner,recipient)/release_slot_idx(owner), armed-gated isr_entry (fall-through, single EOI), claim_slot IrqGuard-serialized, drain_task, stateful line-mask restore.
- src/kernel/syscall/syscall_handlers_irq.cpp: SYS_IRQ_REGISTER(57)/SYS_IRQ_WAIT(58) — wait mirrors Semaphore::wait (no sti/hlt/cli for kernel tasks), both lock scopes revalidate occupied/vector/owner/recipient.
- src/kernel/task/task.cpp: cleanup() → IrqDelivery::drain_task (no dangling recipient/waiter after death).
- src/kernel/test/test_cap_irq.cpp: class cap_irq 12 tests (lifecycle/bounds/single-owner/live-bound/lookup/register matrix/pending-immediate/blocking wakeup/revoked-mid-wait/died-drain/PIC-state).
- docs/specs/cspace.md §2.6 + configuration.md §1.8: IRQ caps implemented, CONFIG_CAP_MAX_IRQ.
- src/kernel/cap/untyped.{hpp,cpp}: exhaustion-model retype (sub-range carve + child Untyped remainder), create_subrange, stretch fail-closed, shared g_live_untypeds bound.
- src/kernel/cap/cap.hpp: retype() doc (sub-range + child, non-destructive pre-check).
- src/kernel/syscall/syscall.hpp + syscall_handlers_cap.cpp: SYS_CAP_RETYPE (56, MAX_SYSCALL=57) on the caller root CNode.
- src/kernel/test/test_cap_untyped.cpp: class cap_untyped 18 tests (carve+child, two-level split, unaligned/oversize rejected, child/parent dispose, full-table precheck, syscall dispatch + validation matrix, live-bound counts children).
- docs/specs/cspace.md §2.8: exhaustion model, child atomicity, stretch fail-closed, SYS_CAP_RETYPE, counts 966.
- src/kernel/cap/mmio.{hpp,cpp}: MmioCap (phys/size/bar_type, CONFIG_CAP_MAX_MMIO, create/create_from_bar).
- src/kernel/memory/vmm.{hpp,cpp}: map_mmio_from_cap/unmap_mmio_from_cap (refuse revoked caps + IO types).
- src/kernel/syscall/syscall_handlers_mmio.cpp: sys_ioport_grant (55) — IO MmioCap coverage + CAP_RIGHT_WRITE; immediate apply via iopb_switch_to.
- src/kernel/arch/hal/gdt.hpp + x86_64/hal/gdt.cpp: TSSBlock (TSS + 8KiB IOPB + 0xFF terminator), iopb accessors, default-deny init.
- src/kernel/arch/hal/iopb.hpp + x86_64/hal/iopb.cpp: per-task IOPB pool (CONFIG_IOPB_MAX_TASKS), claim/grant/switch/release/snapshot_reset; claimed-slot defense.
- src/kernel/task/task.{hpp,cpp} + elf/elf.cpp: iopb_slot_ field + IOPB_SLOT_NONE sentinel at all 4 memset sites.
- src/kernel/task/scheduler.{hpp,cpp}: TaskFields iopb_slot capture/restore; switch hooks.
- src/kernel/test/test_cap_mmio.cpp: class cap_mmio (10 tests).
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
