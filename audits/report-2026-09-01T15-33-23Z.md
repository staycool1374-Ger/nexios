# AUDIT REPORT 2026-09-01T15-33-23Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/arch/hal/iopb.hpp, src/kernel/arch/x86_64/hal/iopb.cpp, src/kernel/cap/mmio.cpp, src/kernel/cap/mmio.hpp, src/kernel/elf/elf.cpp, src/kernel/memory/vmm.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_mmio.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_mmio.cpp, src/kernel/test/test_cap_mmio_user.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS

- [S1] src/kernel/task/task.cpp:1879 — memory safety / use-after-free (drain-after-free ordering)
  WHY: In `TaskControlBlock::cleanup()`, the user PML4 is freed at lines 1844-1845
  (`VMM::free_user_pages(page_table_)` + `PMM::free_page(page_table_)`), but
  `cap::MmioUserMap::drain_task(*this)` runs at line 1879 — AFTER that free.
  `drain_task` walks the stored per-slot `pml4` and calls
  `VMM::unmap_mmio_from_cap` → `map_page_in_pml4` → `get_table(..., create=true)`,
  which, when it encounters a cleared table entry (free_user_pages clears
  `pml4[idx]=0` at vmm.cpp:860), ALLOCATES a fresh table page and writes
  `table[index] = new_page | flags` (vmm.cpp:211) into the freed page-table
  memory.  This is precisely the defect the iter-2 S1 fix was added to prevent —
  but the fix only covered the `exec_into_current` path (drain at elf.cpp:720
  correctly placed BEFORE the old_pml4 free at elf.cpp:782-783).  The `cleanup()`
  path retains the same UAF: a task that is torn down with live MMIO mappings
  (e.g. test `mmio_user_cleanup_drains_maps`) triggers a write into freed PML4
  memory, plus spurious PMM table-page allocations on a dying task.  The mapping
  must be drained BEFORE `free_user_pages`, matching the exec pattern.
  Corrective patch written to `audits/rejected_patch.diff` moves
  `cap::MmioUserMap::drain_task(*this)` into the `if (page_table_)` block,
  immediately after `BufferPool::unmap_all(*this)` and before the PML4 is freed.

## PATCH
`audits/rejected_patch.diff` was written: it relocates `MmioUserMap::drain_task`
in `cleanup()` to run inside the `if (page_table_)` block, immediately after
`BufferPool::unmap_all(*this)` and before `free_user_pages`/`PMM::free_page`,
so the stored slot pml4 is never walked after the PML4 (and its table pages) are
released.

DECISION: REJECTED
