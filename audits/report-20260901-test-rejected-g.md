# AUDIT REPORT 2026-09-01T16-24-40Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/arch/hal/iopb.hpp, src/kernel/arch/x86_64/hal/iopb.cpp, src/kernel/cap/mmio.cpp, src/kernel/cap/mmio.hpp, src/kernel/elf/elf.cpp, src/kernel/memory/vmm.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_mmio.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_mmio.cpp, src/kernel/test/test_cap_mmio_user.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp

## FINDINGS

- [S2] src/kernel/cap/mmio.cpp:213-221 (unmap) — Memory safety / slot-pin double-release (refcount corruption -> premature dispose -> use-after-free).
  WHY: `unmap` captures `mmio`/`pml4` under s_lock_, unlocks, runs `VMM::unmap_mmio_from_cap` OUTSIDE the lock (a preemption point on this UP, preemptible kernel), then re-locks, unconditionally sets `occupied=false`, and unconditionally calls `mmio->release()` — releasing the slot's pin even if a concurrent `invalidate_cap` (cross-task revoke of the shared MmioCap, reachable because caps are shared KernelObjects referenced by multiple cspaces) already cleared that slot and released its pin in the unlocked window. The same captured pointer therefore gets `release()`d twice (release-of-a-slot-not-held). Refcount underflow permits dispose() to run early and MemPool::free the cap while a later path still dereferences `mmio->size` — the exact UAF class the iter-4 slot-pin fix claims to close. The pin-only invariant ("slot pin keeps the cap alive") holds only while the slot stays occupied; a concurrent clearer breaks that assumption. `unmap` takes no per-operation reference of its own and unconditionally releases the pin.
  FIX: re-validate the slot at the clear step and release the pin only if this path observes the occupied true->false transition (see audits/rejected_patch.diff). `invalidate_cap`/`drain_task` clear+release atomically in a single lock section and are safe; `map`'s failure rollback has the same split-capture/clear hazard and is fixed the same way.

- [S2] src/kernel/cap/mmio.cpp:179-187 (map, failure rollback) — Memory safety / slot-pin double-release.
  WHY: after `VMM::map_mmio_from_cap` fails (outside the lock), the rollback unconditionally sets `occupied=false` and calls `mmio.release()`. A concurrent `invalidate_cap` can clear+release the claimed slot while `map` was running unlocked, so the rollback's `mmio.release()` releases a pin it no longer holds (double-release), and its `occupied=false` can clobber a slot that a concurrent `map` has since reused for a different mapping.
  FIX: gate the rollback clear+release on re-validating that the slot still holds this exact mapping (see patch).

- [S3] src/kernel/syscall/syscall_handlers_mmio.cpp:83-92 — Resource accounting note.
  WHY: when `iopb_ledger_add` (or `iopb_grant_range`) fails after `iopb_claim` succeeded, the task's claimed IOPB slot is not released on this path; it is only reclaimed at task cleanup via `iopb_release`. Matches the pre-existing grant-range-failure behavior and is not a permanent leak (cleanup reclaims it), but it can transiently exhaust the CONFIG_IOPB_MAX_TASKS pool within a task's lifetime. Consider `iopb_release` on the failure paths.

- [S3] src/kernel/syscall/syscall_handlers_mmio.cpp:124-128 — note only, not a defect.
  WHY: `sys_mmio_map` releases `obj` (its lookup ref) after `MmioUserMap::map`. The slot pin keeps the cap alive while mapped; the caller ref is correctly dropped. Confirmed correct.

## VERIFICATION OF iter-4 CLAIMS (per brief)
1. Slot-pin lifecycle (acquire at map; released on unmap/invalidate/drain/snapshot_reset/map-rollback): present, but the release is NOT exactly-once under concurrency (S2 above).
2. dispose()-triggered reentrancy: safe — all pin releases occur outside s_lock_; a last-release may run dispose()->invalidate_cap() which re-enters s_lock_ with no lock held, and by then the slot is already cleared so invalidate_cap is a no-op. Confirmed.
3. snapshot_reset collects pins under the lock and releases them outside it: confirmed safe.
4. acquire() during map() cannot trigger dispose: confirmed — the syscall caller holds a ref, so map's acquire only increments; it can never decrement to zero.

## OTHER CRITICAL CHECKS
1. RT-path allocations: none — registry/ledger are static bounded arrays; snapshot_reset uses a fixed-size stack array. OK.
2. Concurrency boundaries: s_lock_/g_iopb_lock are never held across VMM map/unmap or reschedule, and never across a release that may dispose. Lock order scheduler_lock_ -> g_iopb_lock -> s_lock_ has no inversion (no path holds two of them nested). OK apart from the S2.
3. Assertion masking / Heisenbug: the `if (user && phys_addr < PMM::total_memory())` relaxation of the is_user_page ENSURE is a legitimate accommodation for device phys (>= total RAM), not a timing-bug mask; normal user maps below total RAM still ENSURE. No test assertion masks a timing bug. OK.
4. Fork deep-copy skip at all 3 leaf sites (`!PMM::is_user_page` -> dst=0) is fail-closed and correct. OK.
5. Exec/cleanup drain ordering (before old PML4 free) closes the stored-pml4 UAF. OK.
6. User VA window static_asserts (above heap, below stack) are correct for BASE=0x61000000, REGION=2MiB, MAPS=8. OK.
7. IOPB retroactive revoke re-masks bits and reloads the TSS when the affected task is the loaded owner; ledger cleared idempotently in dispose/revoke. OK.

## PATCH
`audits/rejected_patch.diff` was written (applies cleanly to the worktree). It makes `MmioUserMap::unmap` and `MmioUserMap::map`'s failure rollback re-validate the slot (occupied + matching cap/VA/owner) at the clear step and release the slot pin only when that path observes the occupied true->false transition, eliminating the concurrent double-release while preserving release-outside-the-lock (dispose reentrancy).

DECISION: REJECTED
