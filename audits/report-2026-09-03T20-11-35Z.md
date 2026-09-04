# AUDIT REPORT 2026-09-03T20-11-35Z (iteration 3)
PATCH: audits/pending_patch.diff
FILES: docs/specs/ipc.md, docs/specs/shm.md, src/kernel/cap/frame.cpp, src/kernel/cap/frame.hpp, src/kernel/cap/frame_map.cpp, src/kernel/cap/frame_map.hpp, src/kernel/elf/elf.cpp, src/kernel/ipc/shm_ring.hpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_shm.cpp, src/kernel/task/task.cpp, src/kernel/test/test_cap_shm.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_ipc_robustness.cpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp
METHOD: source verification against HEAD (main) + `make debug` compile/link + QEMU runs
  (`cap_shm` 5/5, `cap_mmio_user` 9/9, `cap_mmio` 14/14, `ipc_robustness` 7/7 — all PASS,
  recorded in test-history.txt 2026-09-03 20:11:07).

## FINDINGS

### Previous findings — remediation VERIFIED
1. [S1] `frame_user_map_revoke_denied` release-after-free — **REMEDIATED** (iter-1). The
   trailing `fc->release()` after `cs->remove(slot)` is gone; only the slot ref is dropped
   (remove()'s 1→0 runs dispose). Verified frame_map.cpp:test / cap.cpp:115-135.
2. [S1] `shm_ring_revocation_cleanup` release-after-free — **REMEDIATED** (iter-1). `g_fc` is
   a raw observation pointer; the harness never releases it. Task teardown (release_all_objects
   → CNode dispose → remove) drops the slot ref → dispose; `invalidate_cap` drops the map pin
   at revoke. No `fc->release()` remains in the harness.
3. [S1] **multi-page FrameCap teardown partial — REMEDIATED (iteration-2 corrective patch)**.
   Verified all four sub-checks:
   (a) **count read safe** — `pages = s_slots_[i].fc->count` is captured under `s_lock_` while
       the slot is occupied; the slot's pin (`fc.acquire()`) keeps the cap alive (refcount ≥ 1),
       so dispose cannot have run and `count` is immutable post-create. frame_map.cpp unmap
       (:372), invalidate_cap (:427), drain_task (:457).
   (b) **every page unmapped** — all three sites loop `for (j = 0; j < pages; ++j)
       VMM::unmap_frame_from_cap(va + j*PAGE_SIZE, pml4)`; `VMM::map_frame_from_cap`
       (vmm.cpp:1312-1315) maps exactly `fc->count` pages and `unmap_frame_from_cap`
       (vmm.cpp:1320-1322) clears exactly one PTE — the loop is complete.
   (c) **no off-by-one** — `j < pages` covers indices 0..pages-1 → exactly `pages` PTEs at
       `va + 0*PAGE .. va + (pages-1)*PAGE`.
   (d) **strengthened test genuinely asserts both pages** — `frame_user_map_unmap_roundtrip`
       allocates a 2-page cap, parks the task after SYS_FRAME_UNMAP (does not self-terminate,
       so the cloned PML4 stays live), then asserts
       `virt_to_phys_in_pml4(va, pml4) == 0` **and**
       `virt_to_phys_in_pml4(va + PAGE_SIZE, pml4) == 0` before `terminate_and_drain`.
       PASSED in QEMU (debug `cap_shm` 5/5).
4. [S3] vacuous death-drain — **REMEDIATED** (real PML4 + `g_map_ok` + encoded handle +
   frame alloc/free balance; asserts `g_map_ok==1` and live_count baseline).
5. [S3] bare slot index → real handle — **REMEDIATED** (`encode_handle(cspace_id, slot,
   slot_gen)`; revoke denial genuinely exercised at lookup→acquire refusal).
6. [S3] `is_user` enforcement — **REMEDIATED** (`map` rejects `!fc.is_user`; defense-in-depth
   below the creation-side guarantee).
7. [S3] indentation — fixed.

### Critical checks (re-run on the full patch)
1. **Slot-pin exactly-once (5 registry ops)** — held. claim/acquire under `s_lock_`; every
   release is guarded by the observed `occupied` true→false transition (map rollback, unmap
   clear, invalidate_cap, drain_task) or a collected-pointer list (snapshot_reset releases
   outside the lock). No path double-releases: revalidation checks fc/va/owner/gen before the
   release; a concurrently-cleared slot fails revalidation and releases nothing.
2. **Revocation closure ordering + multi-page completeness** — dispose/revoke call
   `invalidate_cap` before frame free / `KernelObject::revoke` (frame.cpp); all three teardown
   sites now clear every page (see #3 above).
3. **Drain before free_user_pages at both call sites** — task.cpp:1851 (`drain_task`) precedes
   `free_user_pages`/`free_page(page_table_)` at task.cpp:1859; elf.cpp:722 (`exec_into_current`)
   precedes the old-PML4 swap/free. A recycled PML4 is never walked after free.
4. **SYS_FRAME_CREATE rollback** — all failure paths free frames exactly once: cap-create fail
   frees `count` pages; install-fail `fc->release()` → dispose frees them (install already rolled
   back its own acquire on table-full, cap.cpp:108-113); success leaves them owned by the slot.
5. **Syscall table integrity** — `MAX_SYSCALL = 66`; `syscall_table_` initializer has exactly 66
   entries (FRAME_CREATE=63, FRAME_MAP=64, FRAME_UNMAP=65 appended); `handle` bounds-checks;
   `k_syscall_fast[]`/`SYSCALL_FAST_MASK` unchanged — the page-table-touching syscalls stay out
   of the FAST path (issue #92 discipline).
6. **ResourceTracker** — the new code's own accounting balances (cap_object/cap_slot/pmm
   create↔dispose/install↔remove/alloc↔free across all 5 tests). See F-2 for a pre-existing
   shared-path warning.
7. **Lock ordering** — `s_lock_` never held across VMM map/unmap or reschedule (all VMM calls
   outside the guard in every op).
8. **Part A: no IPC logic change; real wake path** — the patch does not modify ipc.cpp; the
   ipc.md claim is accurate against ipc.cpp:506-512 (descending insert) and ipc.cpp:543-559
   (pop head); `IpcPriorityOrderedWake` drives the real `IPC::recv`→`wake_sender` path and
   teardown (`terminate_and_drain3`) precedes the post-reap asserts. PASSED (7/7).
9. **SYS_FRAME_MAP authority** — `cap::lookup` (type Frame + `CAP_RIGHT_WRITE` + generation +
   cspace_id) gates the map; revoked caps refuse at lookup→acquire and again at `map`/VMM;
   `!fc.is_user` rejects kernel-backed caps.

### New findings (this iteration)
- [S3] **src/kernel/syscall/syscall.hpp:95 — style gate failure.** The `FRAME_CREATE` enum
  comment text "allocate contiguous user frames" trips the clang-tidy `memory` check
  ("Dynamic allocation on kernel path") → `make check-style` / `make build` report 1 error and
  `make build` is RED. The C++ build (`make debug`/`make execute-test`, which use the
  `debug`/`release` targets, not `build`) compiles, links, and runs fine. Fix: reword the
  comment to avoid the "alloca" substring. Non-safety, but blocks the documented build gate.
- [S3] **ResourceTracker +1 PMM-page WARN on cap_shm tests (observation).** Each cap_shm test
  that terminates a cloned-user-PML4 task reports `PMM pages before=1090 after=1091`. This is
  the SAME single-page delta already emitted by the merged `cap_mmio_user` class
  (`mmio_user_syscall_dispatch`), i.e. a pre-existing artifact of the shared cloned-PML4 task
  teardown path, NOT introduced by this patch (verified: `cap_mmio` 14/14 and `ipc_robustness`
  7/7 — no cloned PML4 — leak nothing; `cap_shm` leaks exactly the same +1 as `cap_mmio_user`,
  no second page). The harness logs the delta as WARN without failing the test
  (`ResourceTracker::check` return is ignored at test_isolate.cpp:720). Test-2's one-time
  MemPool[7]/Cap-object delta is the lazily-created root CNode of the long-lived init task
  (test artifact, not a leak in the new code).
- [S3] **Revoke-ordering TOCTOU (observation).** `FrameCap::revoke()` runs `invalidate_cap`
  before the `revoked_` store, leaving a narrow window in which a concurrent SYS_FRAME_MAP
  could claim a slot after the scan but before the store. This is byte-for-byte the accepted
  `MmioCap::revoke()` precedent (mmio.cpp:124-129); consequence is bounded (the mapping is
  always cleaned at dispose, pin held until then — no UAF). Not a regression; recorded for
  completeness.

## PATCH
No rejected_patch.diff written — decision is APPROVED. The two S3 items should be remediated
before release: reword the FRAME_CREATE comment (F-1) so `make build`/`check-style` is green,
and track the shared-path +1 PMM page (F-2) as a pre-existing follow-up.

DECISION: APPROVED