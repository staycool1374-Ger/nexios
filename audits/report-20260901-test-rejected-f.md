# AUDIT REPORT 2026-09-01T16-13-13Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/arch/hal/iopb.hpp, src/kernel/arch/x86_64/hal/iopb.cpp, src/kernel/cap/mmio.cpp, src/kernel/cap/mmio.hpp, src/kernel/elf/elf.cpp, src/kernel/memory/vmm.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_mmio.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_mmio.cpp, src/kernel/test/test_cap_mmio_user.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS

- [S2] src/kernel/cap/mmio.cpp:205 (MmioUserMap::unmap) — memory safety / UAF (Critical Check #4)
  WHY: The registry stores a non-owning `cap::MmioCap *mmio` slot pointer that is documented
  "equality-only — never dereferenced", but `MmioUserMap::unmap` (reached from the un-gated
  `sys_mmio_unmap`, which holds NO capability reference) passes it to `VMM::unmap_mmio_from_cap`,
  which reads `mmio->size` (vmm.cpp:1346).  Between releasing `s_lock_` (a non-IRQ-masking
  SpinLock) and that dereference, a timer IRQ can preempt to another task that disposes the
  (shared) cap — `MmioCap::dispose` → `MemPool::free(this)` — after which `mmio->size` reads
  freed memory (a kernel-mode fault or an arbitrary-page-count unmap in the owner's own PML4).
  Because `map()` takes no `acquire()`, the registry holds no reference, so the cap can be freed
  while a live slot still points at it.  `invalidate_cap`/`drain_task` are safe only because their
  callers (dispose/revoke/cleanup) guarantee the cap is alive; the syscall `unmap` path has no such
  guarantee.  `iopb_ledger_clear_cap`'s `owner->iopb_slot_` deref is NOT affected (serialized with
  `iopb_ledger_drop_task` under `g_iopb_lock`, and the ledger is dropped before the TCB is freed).
  FIX (in audits/rejected_patch.diff): the registry holds an `acquire()` reference on the cap for
  the lifetime of each live slot (taken at `map()` under the claim, released at slot-free in
  `unmap`/`invalidate_cap`/`drain_task`/`snapshot_reset`, always OUTSIDE `s_lock_` so a last
  release → dispose → `MemPool::free` never runs under the map lock).  This guarantees `mmio->size`
  is never read after the cap is freed.  Semantics preserved: dispose cannot fire while mapped (the
  registry ref keeps refcount ≥ 1), so `dispose`'s `invalidate_cap` becomes a safe no-op and
  immediate-unmap remains the responsibility of `revoke`/`unmap`/`drain_task`, all unchanged in
  behavior for the existing tests.

- [S3] src/kernel/memory/vmm.cpp:915,953 (x86_64/aarch64) and 1021-1025,1042-1046 (riscv64) —
  preprocessor / conditional semantics (Critical Check #6)
  WHY: The iter-3 deep-copy fix adds the `PMM::is_user_page` skip to the 4KiB leaf sites
  (vmm.cpp:988,1067,1097) but NOT to the 1GiB/2MiB huge-page "copy-as-is" branches.  This is not
  reachable through the current MMIO window (map_page_in_pml4 / map_mmio_from_cap create only 4KiB
  leaves), so it is not a live defect; flagged as a hardening note so a future huge-page device
  mapping does not silently deep-copy device phys into a forked child.

## PATCH
audits/rejected_patch.diff was written (git-apply-able, applies to the current worktree).  It makes
the MmioUserMap registry hold an `acquire()` reference on each mapped MmioCap for the lifetime of a
live slot (released outside `s_lock_`), closing the syscall-path use-after-free in `unmap()` and
making `snapshot_reset` release the slot pins safely.

DECISION: REJECTED
