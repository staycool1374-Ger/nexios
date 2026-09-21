# AUDIT REPORT 2026-09-03T19-55-42Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/ipc.md, docs/specs/shm.md, src/kernel/cap/frame.cpp, src/kernel/cap/frame.hpp, src/kernel/cap/frame_map.cpp, src/kernel/cap/frame_map.hpp, src/kernel/elf/elf.cpp, src/kernel/ipc/shm_ring.hpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_shm.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_shm.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_ipc_robustness.cpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S1] src/kernel/test/test_cap_shm.cpp:169 — use-after-free / double-release in `frame_user_map_revoke_denied`: `cs->remove(slot)` drops the slot reference (1->0 -> `FrameCap::dispose` -> `MemPool::free(fc)`), then `fc->release()` decrements the refcount of the already-freed block.
  WHY: The creator reference was already dropped inside `install_frame`, so the slot holds the only reference; `remove()` triggers dispose, making the trailing `fc->release()` a write to freed kernel heap (debug builds 0xDD-poison it, mempool.cpp:139) that corrupts a live object's refcount if the block is recycled — memory-safety violation.
- [S1] src/kernel/test/test_cap_shm.cpp:451 — use-after-free in `shm_ring_revocation_cleanup`: after `terminate_if_live`/`wait_for_termination_safe`/`drain_zombie_list`, the task's `release_all_objects` (task.cpp:1890) disposes the CSpace CNode -> slot remove -> FrameCap ref 1->0 -> `MemPool::free(fc)`; the harness then calls `fc->release()` on the freed block.
  WHY: The harness holds no reference of its own (g_fc is a raw pointer), so the trailing release writes to freed memory; if the block is recycled in the window, a live object's refcount is decremented -> premature dispose -> real UAF.
- [S3] src/kernel/test/test_cap_shm.cpp:341 — `shm_ring_task_death_drain` is vacuous: the task is created without a user PML4, so `FrameUserMap::map` returns -1 (`task.page_table_ == 0`), no slot is ever claimed, and the live_count-baseline assertion passes trivially — `drain_task` is not exercised by this test.
  WHY: A kernel task has no user page table; the drain is only genuinely tested by `shm_ring_producer_consumer`; fixed in rejected_patch.diff by cloning a real PML4 and asserting the map actually established a slot.
- [S3] src/kernel/test/test_cap_shm.cpp:163 — `frame_user_map_revoke_denied` passed the bare slot index as `cap_handle` instead of an encoded handle; lookup can then fail for the wrong reason (cspace_id/generation mismatch), so the test may pass without genuinely exercising revoke-denial.
  WHY: A raw slot index only decodes to a valid handle when cspace_id==0 and slot_gen==0; fixed in rejected_patch.diff by encoding cspace_id+slot+generation.
- [S3] src/kernel/cap/frame_map.cpp:281 — the map path does not enforce `FrameCap::is_user`; a kernel-backed (is_user==false) cap would be user-mappable via SYS_FRAME_MAP if it ever reached a user CNode (privilege boundary not enforced at the map layer).
  WHY: Not reachable today — SYS_FRAME_CREATE always installs is_user==true and production never grants Untyped caps (UntypedMem::create with is_user=false is test-only) — so this is defense-in-depth; fixed in rejected_patch.diff.
- [S3] src/kernel/syscall/syscall.hpp:313 — cosmetic indentation regression on the `sys_mmio_map` declaration (`static uint64_t sys_mmio_map` at column 0, no leading spaces).
  WHY: Non-functional style drift in an otherwise consistent declaration block.
- [S3] src/kernel/cap/frame_map.cpp:447 — `snapshot_reset` clears slots and drops pins without first unmapping the stored PML4 PTEs (parity with `MmioUserMap::snapshot_reset`, mmio.cpp:305).
  WHY: Test-only path; safe only because the isolation invariant guarantees no live task holds a mapping at snapshot time; a parked task's PTE would otherwise point at freed/recycled PMM memory.

## PATCH
audits/rejected_patch.diff was written (verified `git apply`-clean on top of the pending-patch state, and both edited files compile with the kernel's `-Wall -Wextra -Werror` flags). Intent: remove the two use-after-free releases (test 2 and test 5), make `shm_ring_task_death_drain` non-vacuous (real user PML4 + asserted mapping), encode a real capability handle in `frame_user_map_revoke_denied` so it genuinely exercises revoke denial, and enforce `FrameCap::is_user` in `FrameUserMap::map` (kernel-backed caps are never user-mappable).

DECISION: REJECTED