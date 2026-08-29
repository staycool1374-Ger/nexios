# AUDIT REPORT 20260829T175957Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/cap/untyped.hpp, src/kernel/cap/untyped.cpp, src/kernel/cap/cap.hpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_cap.cpp, src/kernel/test/test_cap_untyped.cpp, src/kernel/test/test_expected_counts.hpp, docs/specs/cspace.md

## FINDINGS
- [S2] src/kernel/cap/untyped.cpp:198 — child Untyped installed with the caller's `rights` verbatim, but the documented contract (docs/specs/cspace.md §2.8.2: "the child is itself retypable") requires the child slot to carry `CAP_RIGHT_WRITE` (retype's mandatory right, enforced by the `CAP_RIGHT_WRITE` lookup at untyped.cpp:136 and verified by the existing `retype_requires_write_right` test).
  WHY: The patch's own test `retype_two_level_split_child_retyped_again` carves the parent with `CAP_RIGHT_READ` only (`test_cap_untyped.cpp:627`), so the child slot gets READ-only rights; the subsequent `cap::retype(&node, ch, ...)` on the child fails the WRITE lookup (`slot.rights & WRITE != WRITE`), returns -1, and `JARVIS_ASSERT(r2 >= 0)` fails — the test cannot pass as submitted and the "retypable child" contract is broken for any carve whose rights omit WRITE. The caller necessarily holds WRITE over the parent region (the parent lookup demanded it), so granting the child WRITE exceeds no authority.

- [S3] src/kernel/cap/untyped.cpp:60-83 — `create()` now allocates the PMM region BEFORE the `CONFIG_CAP_MAX_UNTYPED` live-bound check (previously checked first), so a bound-exhausted create PMM-allocates then immediately frees `pages` frames.
  WHY: Behaviorally correct (frames are freed on the failure path) and no leak, but a minor efficiency regression versus the original check ordering; acceptable.

- [S3] src/kernel/cap/untyped.cpp:199-203 — the slot-capacity pre-check reads `occupied_count()` without taking `cspace->lock_` (the header documents the spinlock-guarded slots), and the check is advisory.
  WHY: `CNode::install` still enforces the real bound under the lock, and the install-failure path is fail-closed and memory-safe, so a stale count can at worst cause a conservative -1 or defer to the (safe) install-failure path; no safety impact. Same non-locked read pattern as the existing `occupied_count` helper.

No S1 findings: all fail paths (child-creation stretch at untyped.cpp:184-189, target-install failure at 191-197, child-install rollback at 199-208) free each frame of the region exactly once (verified against KernelObject refcount semantics, CNode::install's acquire-rollback, CNode::remove's deferred release, FrameCap::dispose, UntypedMem::dispose, and the g_live_untypeds + ResourceTracker cap_objects/cap_slots accounting); claim_once is a CAS single-winner; lookup pins the parent across the transfer so dispose cannot run mid-retype; retype is task-context only (no IRQ reentrancy, matching existing grant/copy/mint); no dynamic heap allocation outside the bounded MemPool; no assertion masking (the inverted `retype_oversize_rejected_parent_intact` reflects a legitimate semantic change, and the new occupied-count asserts strengthen rather than mask); no new preprocessor/conditional asymmetry.

## PATCH
`audits/rejected_patch.diff` was written: installs the child Untyped with `rights | CAP_RIGHT_WRITE` so the child always remains retypable per the documented contract and makes `retype_two_level_split_child_retyped_again` pass.

DECISION: REJECTED