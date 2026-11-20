# AUDIT REPORT 2026-11-20T20:37Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/cap/carve_binding.cpp, src/kernel/cap/carve_binding.hpp, src/kernel/cap/carved_object.cpp, src/kernel/cap/frame_foreign_map.cpp, src/kernel/cap/frame_foreign_map.hpp, src/kernel/cap/vspace_errors.hpp, src/kernel/cap/vspace_map.cpp, src/kernel/cap/vspace_map.hpp, src/kernel/elf/elf.cpp, src/kernel/log/dmesg_catalog.hpp, src/kernel/memory/vmm.hpp, src/kernel/memory/vmm_clone.cpp, src/kernel/memory/vmm_map.cpp, src/kernel/nexios_config.h, src/kernel/proc/proc_fork_carve.cpp, src/kernel/proc/proc_fork_carve.hpp, src/kernel/proc/proc_svc.cpp, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_errors.hpp, src/kernel/syscall/syscall_handlers_vspace.cpp, src/kernel/task/task.cpp, src/kernel/task/task.hpp, docs/specs/microkernel-transition.md

## FINDINGS
- [S1] src/kernel/proc/proc_fork_carve.cpp:335 — fail() releases the composer TCB pin before destroy() touches the TCB (use-after-free)
  WHY: CarvedObjectCap::dispose frees the region synchronously on the last release; on the begin/bind_tcb_cap failure paths no binding pin exists, so releasing tcb_local first frees the region that destroy() then reads (magic/id/generation) and tears down — reachable on binding exhaustion (64 slots).
- [S2] src/kernel/syscall/syscall_handlers_vspace.cpp:287 — sys_vspace_activate merges the kernel half before any ACTIVATED/ACTIVE_SPACE check and validate_subtree never consults the activation flag
  WHY: probe_publish_locked is idempotent-true for a live root, so nothing bars a second activation of one root onto another unpublished target (two TCBs aliased onto one address space), and merge_kernel_half runs on a possibly-live root; INV-VS-3-prime(b) requires the ACTIVATED refusal with merge after it.
- [S2] src/kernel/cap/carve_binding.cpp:340 — activate_err re-validates the subtree but never refuses an already-activated root
  WHY: same aliasing hole at the internal surface (defence in depth behind the syscall pre-check); probe_publish_locked succeeding idempotently is the exact gap.
- [S2] src/kernel/cap/frame.cpp:86 — FrameCap::revoke (and dispose :64) never calls ForeignFrameMap::invalidate_cap
  WHY: revoke clears only FrameUserMap + pager ledger, so a revoked frame stays readable/writable in the carved child's space until teardown drain — a revocation bypass (binding pin prevents page UAF, authority still leaks).
- [S2] test-history.txt (2026-11-20 proc_elf 134/1 rows) — Stage C gate carries unexplained failures; the 134/1 signal exoneration is testimony-only
  WHY: check-11 requires evidence, not claims; the MAX_SYSCALL pin failures are mechanically explained in-tree (test_cap_tcb.cpp:2145 pins handle(103)==-1, broken by design at 103/104+105) but the BLOCKED-spin/PM-only-repro exoneration has no attached artifact in the submission.
- [S3] audits/plan-564-stageC.md:152 + src/kernel/proc/proc_fork_carve.cpp:194 — INV-CARVE-9 claims no CONFIG_ARCH_* block in any new TU, but push_clone_frame duplicates the three arch register-frame blocks
  WHY: mechanism is sound (duplication with provenance, clone stays identical) but the invariant text is literally false; reword to arch-parity-by-duplication.
- [S3] audits/plan-564-stageC.md:67,152 — plan names carved_region_phys_/carved_pages_ fields that do not exist in code
  WHY: folded per the task.hpp C3-D1 comment (region identity is the TCB address itself); plan/code drift, code is authoritative.
- [S3] src/kernel/task/task.cpp:3427 — CLONE arm frees the PMM page before the caller frees the context ID (plan says statement-identical order)
  WHY: safety property preserved (ID released after the free_user_pages cross-CPU purge), but the text overclaims identity; harmless.
- [S3] src/kernel/cap/vspace_map.cpp:1691 — invalidate_table/vspace split the registry clear and the zero into two lock holdings around the shootdown
  WHY: a concurrent link() in the window loses at worst fail-closed (zeroed entry skipped by validators, orphaned record cleared at next invalidate, re-link refused not misdirected); no wrong-mapping shape, pre-existing lost-update class only.

## VERIFIED (evidence-backed, no finding)
- 104 gate (magic/liveness/not-caller/is_user_/not-RUNNING/page_table_==0&&pcid_==0, TARGET_REJECTED otherwise, merge+converge via the same VMM helpers as composer step 5, side-effect-free refusals, +23 only on TARGET_REJECTED/SUBTREE_INVALID outside locks, context_id_alloc last at :350).
- 103 gate (ACTIVE_SPACE + ACTIVATED mirror, is_user kind bar, truncation guard before narrowing, ScopeGuards, aligned/LIMIT/wrap/unmapped-window probe, ForeignFrameMap::map typed faces, double-release revalidation, map-outside-lock + RO pass).
- Binding registry (slot lifecycle FREE/COMPOSING/ACTIVE/DRAINING, single CONFIG_CARVE_BINDINGS bound for index/modulus/scan, flag-phys-gen-flag seqlock with acquire/release pairing, monotonic gen ABA note, bounded scans, pins released after unlock, no scheduler_lock_ anywhere in new code).
- Foreign map (16 slots, claim+acquire under lock, VMM work outside lock, per-page unmap with hole fold-and-ignore, invalidate/drain/snapshot/live_count/is_mapped present).
- Composer order/pinning (TCB→root+merge→top-down tables ≤24→frames+W^X copy+hole punch-out→stack containment→canaries post-install→fd/cwd/FPU inheritance→PMM kstack→install→activate; VMM typed maps incl. PAGE_ALLOC; TU fully #if-guarded; fork selector #else identical with shared tail).
- Teardown (CLONE arm preserved; CARVED arm links-only+foreign-drain+deactivate+ID-free+clear; drain last under claim gate; destroy POOL-free vs CARVED-drain-no-touch with nothing-after discipline; exec routes via release_replaced_root + re-kind CLONE; switch_address_space unmoved).
- Numbering (103/104, MAX 105, faces 1113/1114/1115 + static_asserts), dmesg +22/+23 (worktree catalog shows +21 then +22/+23, no collision), validators (0/1, >=1, carve-requires-PD cross-knob #error present).
- OFF identity (composer TU empty at 0, proc_svc #else leg identical, no new arch semantics, no T-1 MOVE, §15 in-progress, App A Stage B line superseded by Stage C entry).
- Style: tools/validate_style.py --severity error → Errors: 0 (run 2026-11-20, 578 files).
- Retrieval: plan carries graphify queries + dispositions and vault searches (check-7 satisfied as in prior stages); graphify-out/ modified in worktree (update ran).
- r2 S3s applied in code: sole-user-task-publication wording (:260-264), single CONFIG_CARVE_BINDINGS bound, cross-knob validator.

## PATCH
audits/rejected_patch.diff was written and git-apply-checked (APPLY_OK): (1) fail() destroys the child before releasing composer pins (S1); (2) 104 refuses ACTIVE_SPACE/ACTIVATED before merge (S2); (3) activate_err refuses already-activated roots (S2); (4) FrameCap::revoke/dispose close the foreign registry (S2). Developer applies verbatim, re-runs gates, re-submits.

DECISION: REJECTED
