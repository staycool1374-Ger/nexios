# AUDIT REPORT 20260829T181710Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/cap/cap.hpp, src/kernel/cap/untyped.cpp,
src/kernel/cap/untyped.hpp, src/kernel/syscall/syscall.hpp,
src/kernel/syscall/syscall_handlers_cap.cpp,
src/kernel/test/test_cap_untyped.cpp, src/kernel/test/test_expected_counts.hpp

## FINDINGS

Iteration 3. Both iteration-2 S2 findings were re-audited against the corrective
patches and are RESOLVED (verified below). Exhaustive refcount/ownership trace of
every retype() path confirms exactly-once frame frees.

### Resolved iteration-2 findings (re-verified)

- [RESOLVED] `src/kernel/cap/untyped.cpp:210-218` (child-install rollback)
  Prior S2: `remove(idx_target)` alone dropped the slot ref and leaked the
  FrameCap creator ref (MemPool block + carved pages).  Now the rollback is
  `cspace->remove(idx_target)` (slot ref -> fc 2->1), then `fc->release()`
  (creator ref -> 1->0 -> dispose frees [ut->phys, ut->phys+size)), then
  `child->release()` (child creator ref only, install failed so install() already
  released its temp acquire -> dispose frees [ut->phys+size, ut->phys+ut->size)).
  Carve + remainder partition the whole region; each page freed exactly once.
  `ut->release()` only drops the pin (refcount 3->2), so the spent parent's
  dispose() never runs mid-retype.  Verified all four failure frames below.

- [RESOLVED] `src/kernel/test/test_cap_untyped.cpp` `retype_two_level_split_child_retyped_again`
  Prior S2: `find_slot_of(node, Untyped, ut)` deterministically returned the
  still-occupied, spent `child1` instead of the grandchild.  The grandchild scan
  now excludes BOTH spent parents (`obj != ut && obj != child1`), so slot layout
  {s:ut, r1:fc1, child1_idx:child1, r2:fc2, gc_idx:gc} yields gc_idx = gc's slot
  deterministically; phys/size asserts match the prefix carve chain.

### Exactly-once region ownership — all failure frames traced

- [PASS] `FrameCap::create` null (untyped.cpp:171-175): no fc/child exist;
  `reset_claim()` + pin release; guard cleared so the Untyped still owns the
  whole region and frees it exactly once at its own dispose. No leak, no double-free.
- [PASS] child `create_subrange` null, stretch (untyped.cpp:186-191): child
  creation failed inside `alloc_object` BEFORE any object/counter was allocated
  (bound check precedes MemPool::alloc; MemPool::alloc failure frees nothing).
  `fc->count` stretched to `ut->size/PAGE_SIZE`, `fc->release()` -> dispose
  returns the WHOLE region to PMM exactly once. Parent guard stays set (fail-closed);
  parent dispose() later frees nothing. `g_live_untypeds` not bumped for the
  never-created child.
- [PASS] target install fail (untyped.cpp:194-201): child not yet installed
  (creator ref only), `child->release()` frees remainder; `fc->release()` frees
  carve; region partitioned exactly once.
- [PASS] child install fail (untyped.cpp:210-218): as above, RESOLVED.
- [PASS] success (untyped.cpp:221-226): both slots hold refs; creator refs
  dropped; each page owned by exactly one of {fc, child}; parent spent.

### Re-verified checklist

- [PASS] `rights | CAP_RIGHT_WRITE` child install (untyped.cpp:208-209):
  authority-sound. retype() already demands CAP_RIGHT_WRITE on the parent slot,
  so the caller held WRITE over the entire parent region; the child covers only
  a sub-range of it and is installed into the caller's OWN cspace. WRITE on the
  child (required for the exhaustion model, else the child would be
  un-retypable) grants no authority the caller did not already possess over that
  physical region. No escalation; documented design.
- [PASS] ResourceTracker / `g_live_untypeds` balance: alloc_object bumps both
  exactly once per object; dispose() decrements + track_remove exactly once;
  every created object (fc/ut/child) is either slot-installed or released on
  every path. New tests all assert zero delta for pmm_pages_used/cap_objects/
  cap_slots (+tasks for syscall tests). `create_subrange` never bumps the bound
  on failure.
- [PASS] `create()` bound-check ordering (untyped.cpp:74-93): pre-check before
  PMM carve (avoids carve-then-free); `alloc_object` re-checks as the shared
  enforcement for `create_subrange`; on the double-checked failure `create()`
  frees the carved pages exactly once (alloc_object failed before taking
  ownership). No double-free.
- [PASS] syscall table sizing (syscall.hpp): CAP_RETYPE=56, MAX_SYSCALL=57;
  `syscall_table_[57]` has exactly 57 initializers (indices 0..56, last =
  `&Syscall::sys_cap_retype`); `Syscall::handle` bounds-checks `number >=
  MAX_SYSCALL` -> -1 before indexing. `sys_cap_retype` (handler) casts the user
  target_type/rights but retype() validates target_type against
  `ut->retype_target` (garbage -> -1, parent intact) and rights are caller-chosen
  for the caller's own cspace.
- [PASS] Two-level test teardown (5 slots + 4 pages) fully balanced; exact-size
  carve now asserted to install ONE slot (`occupied_count == 2`, untyped.cpp:344).
  All 9 new tests registered (cap_untyped 9->18; `all` 957->966) and each removes
  every slot it installed (no stack-CNode slot leaks).
- [PASS] No dynamic heap allocation in critical paths: retype()/create_subrange/
  create() use MemPool (pre-allocated) only; syscall handler adds no `new`.
- [PASS] Concurrency: cspace slots guarded by `SpinLockGuard(lock_)` in
  install/remove/lookup; claim is atomic CAS; `g_live_untypeds`/`retyped_` use
  atomics; `dispose()` never runs under a cap spinlock (remove releases target
  outside lock). No RAII IrqGuard regression introduced (task-context only paths).
- [PASS] No assertion masking: the corrected grandchild scan is a legitimate
  deterministic test fix, not a Heisenbug suppression. No preprocessor #ifdef
  asymmetry in the changed code.
- [PASS] `make build` green + check-style Errors: 0 (developer-reported; static
  confirmation of the diff only — no QEMU/tests run by this auditor).

- [S3] `src/kernel/cap/untyped.cpp:154` — occupied-count pre-check reads
  `occupied_count(cspace)` without `cspace->lock_` (advisory, as previously
  assessed). WHY: on SMP a concurrent install could make the pre-check stale,
  but the post-claim install-failure paths now fully clean up (remove + release
  both objects; whole region returned exactly once), so the worst case is a
  properly-handled failure or a spurious rejection — no leak, no double-free.
  Confirmed S3-acceptable; the rollback fix strengthens this.
- [S3] `src/kernel/cap/untyped.cpp:208-209` — child installed with
  `rights | CAP_RIGHT_WRITE` unconditionally. WHY: required for the exhaustion
  model and authority-sound (caller held WRITE over the parent region), but a
  future delegation path should document that the child always carries WRITE
  even if the caller passed a READ-only rights mask. No action required now.

## PATCH
(no rejected patch — APPROVED)

DECISION: APPROVED