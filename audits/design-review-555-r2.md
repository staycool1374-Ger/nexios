# DESIGN REVIEW — #555 F2: split CAP_DELETE (N6) from CAP_REVOKE (53) — r2

**Plan:** `audits/plan-555.md` **rev 2** (base `main @ 66e698281` = #554 F1 tip).
**Prior verdict:** `audits/design-review-555-r1.md` (REJECTED, 7 findings: 1×S1, 3×S2, 3×S3).
**Method:** static design analysis only (no code exists; no tests run per PROMPT-audit).
Base re-verified this session: `git rev-parse HEAD` = `66e698281`; tree unmoved since r1.

## R2 FIX VERIFICATION (one per r1 finding)

- **S1 drain-then-remove — FIXED (verified real).** Steps 8c–8f now implement strict
  post-order: an item's slot is removed only after `take_child` on that item reports
  exhausted (8d), and `is_named_root` is never removed (8d root exemption, §4.3).
  Proof check (INV-DERIV-9): (a) base holds — a leaf has no child edges, so immediate
  removal destroys only an underived object; (b) step holds — `take_child` is a full
  linear scan consumed destructively, so "reports exhausted" implies every edge with
  that parent is taken, and DFS (LIFO stack, loop to `sp == 0`) fully processes each
  pushed child before the parent resumes draining; hence grandchild edges are always
  taken before any ancestor prune. (c) Step-5 corollary holds: at removal time of slot
  X, all parent-X edges are consumed (drained) and the X-as-child edge was consumed
  at push, so `forget_slot` inside `remove`/`remove_if` matches nothing live and is
  behaviour-neutral during the walk, remaining the leak-closer for
  delete/dispose/teardown. (d) The 8-link depth test genuinely discriminates: under
  the r1 order the `(N1,i0)→(N2,i1)`-family edges are pruned by the eager
  `child_node->remove(child_slot)`, so per-node `occupied_count` assertions fail;
  under INV-DERIV-9 every `Ni` slot is drained then destroyed. Stack arithmetic
  holds: 8 links ⇒ 9 items (root + 8) = exactly `CONFIG_CAP_MAX_DEPTH + 1`; creation
  bound (step 7/9, `child_depth ≤ 8`) keeps it reachable-but-bounded.
- **S2-1 revoke-time contract — FIXED (verified real).** Revoke now implements the
  §4.3 `:253` clause instead of deferring it: per-call `kRevokeEdgeBudget = 64`,
  typed `CAP_ERR_REVOKE_INCOMPLETE` (code 17, X-macro append after 15/16 — last
  today is `INSTALL_FAILED` 14, verified `cap_errors.hpp:32-47`), abort-before-take
  (8d checks `budget == 0 && has_child` *before* `take_child`, so every not-yet-taken
  edge survives the abort), INV-DERIV-11 monotone/resumable. Convergence arithmetic:
  saturated 256-entry table ⇒ ≤ ⌈256/64⌉ = **4 calls**; per-call work ≤ 64 takes ×
  256 scan = ≤ 16 384 comparisons; loop iterations ≤ takes + pops ≤ 2B+2 < guard
  2B+4 = 132. The 124-edge test (2×62) converges in exactly 2 calls with `named`
  slot live throughout and the syscall-face mapping (`GENERIC_FAIL` then `OK`)
  asserted. Q2/step-16 residual is precisely framed: binary owner question
  (creation-depth (a) vs revoke-depth (b)), exact delta confined to step 7, step 8e
  and the depth-test first half, with an explicit "re-plan, not patch" guard because
  variant (b) widens the S1 profile. No normative SHALL is left owner-undecided on
  the implementation path.
- **S2-2 validation locking — FIXED (verified real).** 8a validates **and pins**
  (`acquire`) in one `cspace->lock_` critical section mirroring `lookup()`
  (`cap.cpp:169-195`, verified) / `cap::revoke()` (`:203-220`, verified); the walk
  uses only the captured `(serial, slot, gen)` triple; 8g re-validates under the
  same lock (occupied ∧ gen ∧ same target) and returns `CAP_ERR_BAD_HANDLE` on
  mismatch — deny-never-degrade (INV-MK-9). Every walk-side destroy goes through
  gen-checked `remove_if` (8d; specified in step 5 alongside `remove`), so a
  mid-walk child-slot recycle ⇒ `remove_if` false ("already gone"), never a foreign
  deletion; a mid-walk named-slot delete ⇒ `forget_slot` prunes edges (take finds
  nothing further) and 8g denies — no wrong-success. No remaining TOCTOU widens a
  match or deletes a foreign cap. Deterministic half covered by
  `cap_revoke_stale_named_slot_fails_closed`.
- **S2-3 rollback — FIXED (verified real).** Step 9 hoists `idx_child` above
  `if (child)` (defeats the `untyped.cpp:298` scoping verified at tree), records
  both self-edges with generic `child_depth(parent)+1`, and on either record failure
  executes `remove(idx_target)` **and** `remove(idx_child)` *before* the two creator
  `release()`s. Exactly-once disposal: each `remove` drops one slot ref, each
  `release` drops one creator ref; prefix disposed once via `target`, remainder once
  via `child` — the `untyped.cpp:261-279` ownership invariant is preserved and no
  live child Untyped survives (the failure mode is fully closed, matching the
  `:300-310` no-partial-grant precedent). `forget_slot` inside `remove` prunes the
  first-recorded edge on the second-fails path, so no dangling entry. Probes
  (`occupied_count` unchanged, slot-enumeration for stray Untyped, delta 0) are
  adequate.
- **S3 lock rank — FIXED (verified real).** `g_derivation_lock` is
  `sync::SpinLock{LockRank::Leaf}`; `Leaf = 3` exists (`spinlock_order.hpp:38`,
  verified) and the ranked ctor exists (`spinlock.hpp:44`, verified), so no `sync/`
  change is needed as claimed. M2 check semantics verified
  (`spinlock_order.cpp:144-166`: violation iff outer ≥ inner, both ranked): any
  future nesting of `derivation_lock_` inside another `Leaf` (3 ≥ 3) or above a
  `Sched`/`Queue` lock (3 ≥ 1/2) now *reports* instead of passing silently; correct
  outer→inner order (1/2 → 3) stays quiet. Never-together-by-construction is
  preserved (record after install-return, forget after guard-exit, take/lookup
  sequencing) — the rank is pure hardening with no behaviour change.
- **S3 anchors — FIXED (verified at tree).** `depth_` **is** written
  (`cap.cpp:87`, read this session); only `parent_` is write-free (`cap.hpp:63`).
  Arch paths corrected with directory and verified: `test_aarch64.cpp:1223` and
  `test_riscv64.cpp:689` both pass the `MAX_SYSCALL` **symbol** (verified via grep),
  so 98 → 99 keeps both probes meaningful.
- **S3 spec traceability — FIXED (specified, not deferred).** Step 16 files the
  owner-gated follow-up with five verbatim edits (T-1 heading 0–98 + row 98; N6 row
  98; §3.4 `47+5+46+1 = 99`, certified 55 → 54; INV-MK-7 flip with evidence; §4.3
  bound pinning: creation `CONFIG_CAP_MAX_DEPTH = 8` + per-call budget 64 +
  `INCOMPLETE`/resume). Step 15 states the table-full availability edge (shared
  256-entry table ⇒ busy granter can fail previously-working grants fail-closed
  with `DERIVATION_FULL`), the ≥4×64 rationale, the debug high-water observability
  hook, and the F9/#562 re-home — with the `used()`/high-water counter (no new
  `ResourceTracker` counter per INV-DERIV-7) as sizing input.

## REGRESSION (r1 verified-and-clear intact; no scope change)

- All r1 clear items carry over unchanged per §"Unchanged from r1" (line 19): stack-CNode
  value-only edges, `cap::revoke` preservation (`ramdisk/ramdiskd.cpp:163` — note the
  `ramdisk/` directory elided in both reviews' shorthand; substance verified), retype
  memory safety, `snapshot_reset` ordering/be-fore-`restore_pool_data` prerequisite,
  syscall/MAX_SYSCALL/SYS_MAX arithmetic (`syscall.hpp:127-128` MAX=98, REPLY_RECV=97;
  `syscall.cpp:139,190` MAX-bounded, verified; `test_syscall.cpp:1052` pins only
  `> 79`, verified), mint-via-`do_copy_pinned` recording coverage (`cap.cpp:345-351`,
  verified), arch parity by construction, scope discipline (no F4/F5/#50 smuggling;
  table is the explicit #50 seed), QE handoff shape. Anchors re-verified at the
  unchanged base (`HEAD == 66e698281` this session); counts consistent
  (`cap_lifecycle` 8→13, `cap_syscall` 9→12, `capability` 161→169 = +8 = 5+3 new tests).
- No new S1/S2 introduced by the new machinery: budget counter is `uint32_t` with the
  loop hard-capped by `kRevokeMaxSteps` (see S3-O1 for a ±1 budget-accounting race
  that the step guard absorbs); `remove_if` is specified gen-checked with release
  outside the lock (same shape as `remove`, `cap.cpp:118-138`, verified); 8g
  re-verify runs before the 8f pin-release epilogue; the `INCOMPLETE` path removes
  nothing further and maps to the frozen `SYS_ERR_GENERIC_FAIL` face (Q5 records the
  surfacing question as spec-follow-up, not implementation). `grant()`'s deferred
  `clear_grant` (after record) strictly narrows the failure window: record failure
  now consumes no mint-once authority.

## NON-BLOCKING OBSERVATIONS (S3, no action required pre-merge)

- **S3-O1 (budget accounting race, absorbed).** `has_child` and `take_child` each take
  the derivation lock separately; a concurrent `record` between them could let a take
  succeed with `budget == 0` and wrap the counter. Impact is nil: takes stay bounded
  by the `kRevokeMaxSteps` loop guard (≤ 132 iterations regardless), so at most one
  extra edge per call is destroyed — progress stays monotone and `INCOMPLETE`
  semantics hold. Optional hardening: re-check `budget` after a successful take.
- **S3-O2 (concurrent grant-during-revoke linearisation, safe direction).** An edge
  recorded mid-walk (parent still on the stack, child slot freshly installed with a
  new gen) will be swept by the in-flight revoke. Safe: the edge is a true
  derivation (gen-checked destroy, no foreign deletion); the alternative (missing it)
  would be the unsafe direction for a safety function. The spec is silent on this
  race; no design change proposed.
- **S3-O3 (nits).** Both reviews cite `ramdiskd.cpp:163`; the tree path is
  `src/kernel/ramdisk/ramdiskd.cpp:163` — shorthand only, substance verified.
  Step 7's grant sentence ("that failure path then only calls `dst->remove`") is
  unambiguous in context (the general record-failure sentence already orders pin
  release), but the implementation diff should keep the `target->release()` on that
  path visible to the reviewer.

DECISION: APPROVED
