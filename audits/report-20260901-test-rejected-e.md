# AUDIT REPORT 2026-09-01T15-56-53Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/arch/hal/iopb.hpp, src/kernel/arch/x86_64/hal/iopb.cpp, src/kernel/cap/mmio.cpp, src/kernel/cap/mmio.hpp, src/kernel/elf/elf.cpp, src/kernel/memory/vmm.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_mmio.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_mmio.cpp, src/kernel/test/test_cap_mmio_user.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS

### iter-3 S1 fix verification (task.cpp cleanup drain) — CORRECT
- [OK] Drain-before-free: `cap::MmioUserMap::drain_task(*this)` sits at task.cpp:1847 inside `if (page_table_)`, after `BufferPool::unmap_all` (1836) and BEFORE `VMM::free_user_pages` (1855) / `PMM::free_page` (1856). No slot's stored pml4 can be walked after the PML4/PDPT/PD/PT pages are released.
- [OK] Cap-reference validity: drain runs before `release_all_objects` (task.cpp:1886); `VMM::unmap_mmio_from_cap` dereferences `mmio->size` (vmm.cpp:1327), and the cap block is guaranteed live at drain time. The double-drain defect of iter-3 is gone: exactly one `drain_task` in cleanup (the ~1879 duplicate was removed).
- [OK] Exec drain intact / no double-drain: `exec_into_current` drains at elf.cpp:720 BEFORE `old_pml4` is freed (elf.cpp:781-784); cleanup then finds no task-owned slots (exec cleared them), so no double-unmap. A task that re-maps after exec is handled by cleanup's drain.
- [OK] Kernel tasks: `MmioUserMap::map` returns -1 for `page_table_ == 0`, so no kernel task can own a slot; cleanup's drain is inside `if (page_table_)`, so kernel tasks are untouched.
- [OK] All PML4-teardown paths route through cleanup (`destroy`→`cleanup`, reaper `cleanup`, `TaskPtr` dtor); no direct `free_user_pages(page_table_)` bypass observed without a preceding drain.

### Re-check of CRITICAL CHECKS (patch scope)

- [S2] src/kernel/memory/vmm.cpp:985-988 — fork deep-copy of the new user MMIO window reads/memcpys device phys.
  WHY: SYS_MMIO_MAP installs device-BAR PTEs (phys >= `PMM::total_memory()`) in the user PML4 at CONFIG_USER_MMIO_VA_BASE (0x61000000, PML4 index 3 < `PML4_USER_COUNT`=256). A user driver holding a live mapping that calls SYS_FORK drives `TaskControlBlock::clone` → `VMM::deep_copy_user_pages`, which walks the window and `__builtin_memcpy(HHDM_OFFSET + device_phys, …, 4096)` (vmm.cpp:985-988; riscv64 leaves 1059-1062/1085-1089). Real PCI BAR phys is not covered by the kernel's HHDM direct-map (the kernel explicitly maps only RAM + APIC/IOAPIC pages, vmm.cpp:40-104, apic.cpp:76): the read raises a kernel-mode #PF → `panic("CPU EXCEPTION")` (kernel.cpp:1532) — a user-triggerable kernel crash — and even when mapped, reads device registers (MMIO side effects) and hands the child a RAM snapshot of device state without a capability (violates the feature's own fail-closed contract). This is the first mechanism to place non-RAM phys in a user PML4 (frame caps/buffers/exec were all RAM user pages), so the hazard is introduced by this patch.

### S3 notes (non-blocking)
- [S3] src/kernel/arch/x86_64/hal/iopb.cpp:175-189 + src/kernel/syscall/syscall_handlers_mmio.cpp:83-87 — the ledger entry is reserved BEFORE `iopb_grant_range`; a grant_range failure would leave a phantom ledger entry whose later revoke could re-deny another cap's legitimately granted port.
  WHY: on the current UP kernel claim→ledger_add→grant_range runs inside one IF=0 syscall, so grant_range cannot fail after a successful claim; only the v0.4.4 SMP scope could expose it, and the failure direction is fail-safe (denies ports, no memory unsafety).
- [S3] src/kernel/test/test_isolate.cpp:1355 — `MmioUserMap::snapshot_reset()` sits inside `#if defined(CONFIG_ARCH_X86_64)` while the registry (and SYS_MMIO_MAP/UNMAP) are arch-neutral.
  WHY: non-x86_64 test builds would not reset a leaked registry slot across snapshot cycles; harmless on the x86_64 gate but asymmetric with the arch-neutral design.
- [S3] docs/specs/cspace.md — claims "all registered 1025"; `test_expected_counts.hpp` sets `{"all", 1015, 0, 0}` (1003+4+8).
  WHY: the harness validates against the expected-counts table (1015); the doc figure is stale/wrong (documentation-only).

## PATCH
`audits/rejected_patch.diff` was written (git apply clean) for the S2 finding: in `VMM::deep_copy_user_pages` (vmm.cpp), skip any leaf whose frame is not a `PMM::is_user_page` frame (x86_64/aarch64 PT leaf and the two riscv64 leaves), leaving the child's MMIO-window PTE unmapped — fail-closed, no device-phys memcpy, no kernel #PF, no device snapshot inheritance. After applying, rebuild and re-run `cap_mmio_user` / `cap_mmio` / `process_pml4_clone` / the fork/clone classes, then the full `all` gate; a regression test that forks a task holding a live user MMIO map and asserts the child resolves 0 at the window VA is recommended.

DECISION: REJECTED