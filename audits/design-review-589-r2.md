# DESIGN REVIEW — issue #589 (rv64 isr_nesting baseline normalization, owner option (a)) — R2

PLAN: audits/plan-589.md rev-2 (untracked worktree file, read directly; all six
r1 rejected_patch.diff hunks confirmed present verbatim by content match:
F1 a-fortiori citation, INV-NEST-2 BY-CONSTRUCTION wording, step-9
"INDICATES (does not prove)" + census dump, step-13 CANDIDATE three-way rule,
QE-1 recorded-equality, QE-2 census-gated).
BASE: main tip `3dc3f84da` (branch `main`). Design audit only; commit NOTHING.

Note on patch-file hygiene (out of plan scope, no finding): the worktree
`audits/rejected_patch.diff` is TRACKED+modified, not untracked — HEAD's copy
held an unrelated pmm.cpp patch, now overwritten by the r1 589 patch. The
verbatim-application check below is by content match of all six hunks in the
plan, so it stands regardless; but the overwrite should not ride into a commit.

## R1 FIX VERIFICATION (each fix checked real, not asserted)

### S2-1 — step-13 three-way rule with census exclusion FIRST: FIXED (as close-safety), with an S3 precision tail
Step 13 (plan :53) is now genuinely three-way: leg 1 (handover != 0 → fix),
leg 2 (0/0 → no-repro, keep change), leg 3 (0/nonzero → CANDIDATE → census
exclusion BEFORE any accrual conclusion; held-frames outcome keeps #589 open
WITHOUT a balance follow-up; accrual conclusion only on a held-frame-free
census, with "do NOT present #589 as fixed"). Step 9 (plan :49) no longer
claims proof ("INDICATES (does not prove)"), names the dispatch-order
mechanism (daemons prio 20 outrank init prio 0, taskdefs.cpp:300-314; live
depth 4, syscall_entry.S:228-229), and mandates the census dump. The r1
by-design misfire (leg 3 firing a balance hunt from by-design held frames on
every full boot) is gone: the DEFAULT full-boot outcome now resolves to
"held frames, #589 open, no follow-up" instead of a bogus leak hunt.
HOWEVER — the census method is underspecified (new S3-1, below): the plan
says "dump the task census (all_tasks id/state)" but TaskState
(task.hpp:248-255) offers only undifferentiated BLOCKED/WAITING — id/state
alone cannot establish step 13's qualifier "entered via syscall traps." The
discriminating fields EXIST in-tree and go uncited: `has_utrap_frame`
(task.hpp:1131-1137, set in Syscall::handle, never cleared — exactly the
"entered via trap" predicate) and `blocked_in_recv` (task.hpp:1046-1053,
the recv-channel marker). Worst case under the current wording is UNDER-fire
(any-BLOCKED → exclude → #589 kept open without follow-up, safe direction),
never a false close — hence S3, not a second S2.

### S2-2 — QE-1 asserts recorded-value equality, not == 0: FIXED, no residual contradiction
QE-1 (plan :61) is now `isr_nesting_baseline_recorded`: asserts stored ==
step-13 recorded observed value, with explicit "Do NOT assert == 0" and the
leg-1 rationale (nonzero stored legitimate under firmware carry) plus
mismatch-prints-both-values. Consistent with step 13 in all three legs:
leg 1 legitimizes nonzero stored; leg 2's 0/0 satisfies equality trivially;
leg 3 never touches the stored baseline. QE-2 (plan :62) is gated on BOTH the
#583 proof AND a held-frame-free census with an explicit quiesce precondition
— the r1 "expected-red without quiesce is held frames" trap is now stated in
the handoff. No test-vs-verdict contradiction remains.

### S3-1 (r1) BOOT_ONLY one-shot wording: FIXED
INV-NEST-2 (plan :33) now reads "one-shot BY CONSTRUCTION (single call site)"
and states BOOT_ONLY "does NOT count writes — a second BOOT-phase write would
succeed (verify_and_write gates phase only, global_state.hpp:130-133)."
Matches the r1-verified primitive semantics exactly. Real fix.

### S3-2 (r1) medeleg citation: FIXED
F1 (plan :11) now cites the deregistration (test_riscv64.cpp:1423-1426,
M-mode-only CSR, illegal-insn in S-mode) and reasons a fortiori (S-mode
cannot even observe medeleg; either delegation outcome leaves no residual).
Lines verified in-tree this session at
src/kernel/arch/riscv64/test_riscv64.cpp:1423-1426 (plan omits the
`src/kernel/arch/riscv64/` path prefix — line numbers exact; trivial remark,
no finding).

### S3-3 (r1) release smoke leg for the live ENSURE: NOT ADDRESSED — carried forward (new S3-3)
Grep of the plan for "release" hits only INV-NEST-2's control-flow note
(plan :33). The gate paragraph (plan :65) is debug-only; no rv64 release
`--class none` smoke, no `arch_x86_64` consideration, and no disposition in
OPEN QUESTIONS (plan :78 covers only the step-13 rule). The handover ENSURE
panics in BOTH builds (r1 check 4, assert.hpp:48), so release boots execute a
release-compiled panic path no gate ever exercises. Non-blocking (control
flow identical, QEMU OpenSBI enters SIE==0), but undispositioned.

### S3-4 (r1) cli-first consideration: NOT ADDRESSED — carried forward (new S3-4)
Step 3 (plan :43) is still ENSURE-only; INV-NEST-1 (plan :32) still
asserts-doesn't-establish; no `cli()` belt-and-braces, no disposition.
Exotic-firmware-only robustness (SIE==1 at S-mode entry), gates unaffected —
but undispositioned, so carried.

## REGRESSION CHECK (r1 verified-sound items, all intact)
SBI/mtvec premise with corrected citation (F1); PerCpu authority + F4
layout-only warning in steps 3–5 doc blocks; .Ltrap_park non-accrual (F2);
fix point immediately post-percpu_init_bsp #endif (step 8); BOOT_ONLY
registry row (step 10); verify-only x86_64/aarch64 bodies (steps 4–5);
[NEST] read-only tripwire + census dump (step 9); no-touch list byte-identical
in scope (irq_guard.hpp, park_lock.cpp, scheduler_tick.cpp, *.S — plan :29);
QE-1/QE-3/QE-4 as amended (plan :61/:63/:64); gate set unchanged (plan :65);
steps 12/14/15/16 (build, check-style, diff-patch protocol, LEARNINGS)
present. No regression.

## LOAD-BEARING TRACE (mandate item 5: can any leg present #589 as fixed when it is not?)
- Leg 1 (handover != 0 → "the write IS the fix", close-eligible after
  audit): the measurement genuinely confirms present-at-handover carry and
  the write normalizes exactly that — the close claim is earned FOR #589's
  stated scope. Residual S3-2(new)/S3-4: leg 1 says "record the value"
  (singular) and does not require consulting [NEST]/census, so a CONCURRENT
  post-handover accrual could ride a true carry to closure. Mitigated by the
  permanent tripwire + "after audit" gate, but one sentence ("record both
  numbers + census in every leg; no close-eligibility with an unexcluded
  [NEST] != 0") removes it. S3, not blocking.
- Leg 2 (0/0 → no-repro on this build, keep change + tripwire, say so
  plainly): makes NO fixed-claim; hedged to the build. Single-sample 0 is
  weak given dispatch-order nondeterminism (r1 7b), but the plan does not
  overclaim it. No false-close path. (Remark: state the open/closed
  disposition explicitly on the issue; recommend keep-open with tripwire.)
- Leg 3 (0/nonzero → census-first → #589 stays OPEN on both sub-outcomes):
  false-close is STRUCTURALLY IMPOSSIBLE here — neither sub-outcome closes.
  The only failure mode is misdiagnosis direction (missed vs bogus
  follow-up), bounded by S3-1's one-sentence method cite.
CONCLUSION: no verdict leg can present #589 as fixed when it is not.

## FINDINGS (all S3; no S2)
- [S3-1] Census method underspecified (steps 9/13): name the discriminating
  fields — dump `has_utrap_frame` (+ `blocked_in_recv`) per task alongside
  id/state; define "BLOCKED-via-trap" as `state == BLOCKED &&
  has_utrap_frame` (recv-holders further confirmed by `blocked_in_recv`).
  One-sentence clarification, no semantic change; coder may apply it within
  this approval and record it on the issue.
- [S3-2] Leg-1 close path: require both log numbers + census on #589 before
  close-eligibility (symmetric with leg 2's "record both numbers"); no
  close with an unexcluded [NEST] != 0.
- [S3-3] (carried r1) Add one rv64 release `--class none` smoke for the
  live ENSURE; disposition `arch_x86_64` explicitly (add or decline with
  reason).
- [S3-4] (carried r1) Disposition cli-first explicitly (adopt
  `cli()`-then-ENSURE or decline with the QEMU-OpenSBI SIE==0 rationale).

DECISION: APPROVED (commit NOTHING per mandate). Both r1 S2 blockers are
genuinely fixed in the plan text and verified against the tree; the
three-way verdict rule cannot false-close on any leg; all r1 sound items are
intact. The four S3s are non-blocking and dispositionable at implementation
(S3-1/S3-2) or as recorded follow-ups (S3-3/S3-4).
