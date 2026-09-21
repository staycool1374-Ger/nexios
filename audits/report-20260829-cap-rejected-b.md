# AUDIT REPORT 20260829T181110Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/cspace.md, src/kernel/cap/cap.hpp, src/kernel/cap/untyped.hpp, src/kernel/cap/untyped.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_cap.cpp, src/kernel/test/test_cap_untyped.cpp, src/kernel/test/test_expected_counts.hpp

## FINDINGS

- [S2] src/kernel/cap/untyped.cpp:213-216 — child-install rollback leaks the FrameCap creator reference.
  WHY: `install(fc, ...)` takes an acquire (refcount 2 = creator + slot); `cspace->remove(idx_target)` drops only the slot ref (→1); the creator ref held by `fc` is never released on this path, so the FrameCap MemPool block and its carved pages `[ut->phys, ut->phys + size)` leak permanently (ResourceTracker `cap_objects`/`pmm_pages_used` stay elevated forever). This violates the patch's own invariant ("every frame in a region has exactly one owner at every instant") — after this failure the carved region is owned by an unreachable object — and the spec's step-7 description commits the same omission. The path is reachable whenever a concurrent slot mutation on the same CNode makes the child install fail after the target install succeeded; the unlocked/advisory pre-check (untyped.cpp:154) explicitly permits that window.
  FIX: after `cspace->remove(idx_target)`, add `fc->release();` (drops the creator ref → dispose frees the carved sub-range once; child->release() then frees the remainder once; whole region freed exactly once, no double-free).

- [S2] src/kernel/test/test_cap_untyped.cpp:656-657 — `retype_two_level_split_child_retyped_again` deterministically FAILS.
  WHY: `find_slot_of(node, CapType::Untyped, ut)` returns the FIRST Untyped slot whose object is not `ut` — which is the still-occupied `child1` (fresh CNode slot layout: ut@0, frame1@1, child1@2, frame2@3, grandchild@4). `gc_idx` therefore equals `child1_idx`, so `JARVIS_ASSERT(gc_idx >= 0 && gc_idx != child1_idx)` fails (and had it passed, `gc` would alias child1 and fail the `gc->phys == ut->phys + 2*PAGE_SIZE` / `gc->size == 2*PAGE_SIZE` asserts). The claim that the `rights | CAP_RIGHT_WRITE` fix makes this test pass is false — the test cannot pass as submitted and the change's 18/18 verification claim is broken.
  FIX: scan for the grandchild excluding BOTH spent parents (`obj != ut && obj != child1`).

- [S3] src/kernel/cap/untyped.cpp:154 — the slot-capacity pre-check reads `occupied_count()` without `cspace->lock_` and is advisory. ASSESSMENT: S3-acceptable — `CNode::install` enforces the real bound under the lock and the rollback (once the S2 above is fixed) is memory-safe, so a stale count costs at most a conservative -1. It is, however, the precise window that makes the leaky path-G reachable under concurrency, so the S2 leak fix is mandatory. (S3 from iteration 1, previously REJECTED on — now evaluated.)

- [S3] src/kernel/cap/untyped.cpp:77-79 (create() live-bound check moved before the PMM carve) — RESOLVED. The bound is now checked before carving; the residual race with `alloc_object`'s re-check correctly frees the carved frames on failure (no leak, no double-free).

## PATCH
`audits/rejected_patch.diff` was written (applies cleanly, verified with `git apply --check`): (1) add `fc->release();` to the child-install rollback so the carved sub-range returns to PMM exactly once; (2) fix the two-level split test's grandchild lookup to exclude both the parent and the spent first child.

Verification notes (static): the `rights | CAP_RIGHT_WRITE` child grant is sound — the parent lookup already demands WRITE over the whole region, so no new authority is granted, and it is required for the two-level test's child retype. Stretch fail-closed (untyped.cpp:186-191) verified correct: `fc->count` stretched to the whole region and `fc->release()` frees every frame exactly once with the parent guard set. Target-install failure (untyped.cpp:195-201) verified correct: child/fc creator refs only, install rolled back its acquire, whole region freed once. Validation/pre-check/claim-fail/FrameCap-create-fail paths verified non-destructive. ResourceTracker + `g_live_untypeds` accounting verified symmetric on all non-leaking paths (stretch-test arithmetic re-checked). Syscall table verified: MAX_SYSCALL=57, 57 entries, CAP_RETYPE@56 positionally correct, `handle()` bound-checks. No dynamic heap, no IRQ-context reentrancy, no assertion masking, no ifdef asymmetry.

DECISION: REJECTED