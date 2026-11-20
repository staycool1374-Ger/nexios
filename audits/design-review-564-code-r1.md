# AUDIT REPORT 2026-11-20T164748Z — #564 F11 Stage A (code audit r1)

PATCH: audits/pending_patch.diff
FILES: audits/plan-564.md, docs/specs/microkernel-transition.md,
src/kernel/log/dmesg_catalog.hpp, src/kernel/nexios_config.h,
src/kernel/proc/proc_errors.hpp, src/kernel/proc/proc_mgr_proxy.cpp,
src/kernel/proc/proc_mgr_proxy.hpp, src/kernel/proc/proc_svc.cpp,
src/kernel/proc/proc_svc.hpp, src/kernel/proc/proc_tcb_caps.cpp,
src/kernel/proc/proc_tcb_caps.hpp, src/kernel/proc/proc_wire.hpp,
src/kernel/syscall/syscall_handlers_misc.cpp,
src/kernel/syscall/syscall_handlers_process.cpp, src/kernel/task/boot_sdf.cpp,
src/kernel/task/boot_sdf.hpp, src/kernel/task/scheduler_terminate.cpp,
src/kernel/task/taskdefs.cpp, src/kernel/task/tcb_control.cpp,
tools/validate_style.py

BASE: worktree tip 230997806 (prompt commit on plan base 2275e89c8; prompt
commit is prompt-file-only, no kernel effect). Scope is the uncommitted
production diff only. No test files in the patch (verified: `git diff HEAD`
over src/kernel/test/, src/lib/, initrd is empty). test-history.txt worktree
rows used as gate evidence (append-only, excluded from the patch).
NORMATIVE: audits/plan-564.md rev 3; design verdicts design-review-564-r1
(REJECTED) and -r2 (APPROVED). Method: full pending_patch.diff read (2663
lines) + targeted in-tree reads/greps + two move-purity scripts + one
independent style-validator run. No test execution (per audit role).

## VERIFIED (evidence, not testimony)

- V1 move purity (item 1): script-compared HEAD syscall sources vs new
  proc_svc.cpp. 504/509 removed lines appear verbatim in proc_svc.cpp; the 5
  absent lines are the old file header (2) + 3 replaced `Syscall::` signature
  lines. All 47 added-but-not-in-old lines are new-file boilerplate,
  `ProcSvc::` signatures, or the new Stage B validator `handle_decision`
  (no Stage A caller). The `sys_sigreturn` spacing quirk
  (`uint64_t *regs) {    auto *t`) is preserved verbatim — verbatim-move
  marker. #590 publication/revive-retry (`kWaitReverifyRetryMax`,
  collect_zombie second residence) and #593 trap-funnel kill leg
  (`terminate_err_from_trap` + `switch_away_from_terminating`, INV-593-1
  comment) intact. Wrappers keep exact `Syscall::` arity/types/regs
  forwarding (kill/signal/sigreturn/waitpid name previously-unnamed params —
  benign, forwarding-only); early gates and caller-context mechanism
  untouched. OFF behavior-identity holds (behavior, not byte, per rev 2/3).
- V2 TCB-cap table (item 2): 32-slot fixed array
  (`g_hold_slots[CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS]`, `kMaxHeldTcbCaps` with
  `>=1` / `<= CONFIG_CSLOT_COUNT` static_asserts), generation-keyed
  (pid+generation on hold/lookup/release/funnel), F2 edges. Fresh-root
  self-edge is sound AND precedented: identical arg shape to the #555
  retype self-edges (`untyped.cpp:351-353,357-359`:
  parent=(cspace,slot,gen), child-node=(cspace,kDerivSelfNode,0),
  child=(slot,gen)); comment honestly defers cross-CNode parenting to
  Stage B (record_copy_edge needs granter naming that does not exist yet;
  no creator cap can exist at Stage A since nothing holds). All five entry
  points present. Funnel hook lives in `scheduler_terminate.cpp`
  `publish_task_death` (plan step 7 named scheduler_lifecycle.cpp — the
  function actually lives here; code hooks the true M1 funnel, covering
  prompt + `cleanup()` backstop since the backstop routes through publish).
  Placement verified in situ: after `wake_reply_waiters_of`, IrqGuard held
  (line 64), `ENSURE(scheduler_lock_depth_own()==0)` at entry (line 55).
  Leaf takes only, sequential (table lock released before CNode
  install/derivation record — INV-DERIV-1 respected; DerivationTable methods
  take only the leaf lock). Idempotent (cleared entries skipped on
  re-publish; `remove_if` gen-checked). Zero logging on the funnel leg (sole
  log site is the hold-full path, outside the table lock).
- V3 Q9 (item 3): `tcb_control_authorize` branches 5
  (`parent_id==caller.id`, no cap) and 6 (SUPERVISOR+grantee+target binding)
  read-verified intact; diff touches only the two INV-ADM-15(c) asserts.
  Proxy TU fully `#if CONFIG_PROC_MGR`-guarded (empty TU when 0; header
  decls likewise guarded). Holds use `TcbCap::create` = CREATOR_ONLY
  (`tcb_cap.cpp:82`); `create_supervised` never referenced. Grep:
  zero `tcb_control` references in src/kernel/proc/ outside one comment —
  no gate call site exists, so "PD never calls the gate" holds structurally.
- V4 config/SDF (item 4): `CONFIG_PROC_MGR` default 0 + 0/1 validator;
  numerics (`RPC_TIMEOUT_TICKS` 5000, `MAX_HELD_TCB_CAPS` 32) defaulted
  before the validator group; `CONFIG_PROC_MGR` default after its validator
  — same as the `PROCFS_SRV` precedent (`#if` on undefined reads 0, passes).
  PROC_MGR=13 / count 14 / zero-cap row / `sdf_policy_only` +
  `sdf_zero_caps_allowed` + `pd_owns_device` false-branch / PD-knob guards
  (TU-level, compose-block, reboot-table) + `CONFIG_PROC_MGR` guard all
  present; all evaluate identically at knob 0. tcb_control asserts 14 with
  the F11 revisit note (not mem-mgr, no new exclusion, CREATOR holds are
  tracking-only). taskdefs bound widened to PROC_MGR with NO new row —
  correct for Stage A (row is Stage B).
- V5 dmesg/style (item 5): live TASK users are +1/+7/+9/+10/+17 only; new
  +18/+19/+20 collide with nothing (LEARNINGS #354 honored). Allowlist is
  one line (proc_svc.cpp for the moved fault-atomic SMAP path —
  `g_user_access_recover_ip` + `NEXIOS_FAULT_RECOVERY_KEEP` moved verbatim;
  rewriting would break V1 purity). Independent run:
  `validate_style.py --severity error`: Errors: 0 (572 files).
- V6 OFF identity (item 6): proxy empty TU at knob 0 (verified); funnel is
  one atomic fast-path return on empty table (no holds can exist — no
  production caller); wrappers pure delegation. Behavior-identical claim
  (not byte-identical, per rev 2 OBS-1 fix carried in rev 3) holds.
- V7 stage boundary (item 7): sole production caller of the new API surface
  is the funnel hook (`release_on_death`); hold/lookup/release/
  snapshot_reset/handle_decision/proxy have no production callers. No
  userspace PD, no SDF row task, no dispatch-table change (no new syscall
  numbers), no `CONFIG_ARCH_*` in new code (grep clean), no test files.
- V8 gates (item 8): test-history.txt carries all nine claimed rows, every
  one `FAILED: 0` — selftest 157, proc_elf 135, core_task 66, capability
  200, cap_tcb 6, cap_vspace 13, ipc 106, servers 74, release selftest 87.
  ipc/servers bumps with zero test-file changes in the diff is consistent
  with QE testbed merges on the tip-forward base. `arch_pause` confirmed in
  `src/kernel/test/test_hal.cpp` (test file, read-only lane) — the
  aarch64/riscv64 link-blocker claim is structurally plausible;
  production code is arch-neutral (V-arch checks above). #596
  pristine-base proof not independently corroborated (`gh` returned empty
  here — limitation, see S3-c).
- V9 QE gap (item 9): `snapshot_reset` is unwired — no production caller and
  no `test_isolate.cpp` hook (grep: no proc refs) — confirmed as the flagged
  testbed-lane handoff item, not a code defect.
- Criticals 1/4/5/9/10: funnel leg allocates nothing (stack-fixed
  `FunnelJob[32]`); hold-path pool alloc is bounded + nullptr-checked;
  no double-free (index cleared under lock before the single gen-checked
  `remove_if`; holder-dead leg index-only); no reschedule/switch/logging in
  funnel; every refusal typed fail-closed incl. trailing `BAD_REQUEST` for
  out-of-range ops and `STOP_REFUSED` pre-policy. Check-7 satisfied via the
  plan's carried retrieval section (9 graphify + 3 vault searches with
  dispositions, plan-564.md lines 14-23).

## FINDINGS

- [S3] src/kernel/proc/proc_tcb_caps.cpp:hold (claim-then-publish) — free-slot
  index is claimed under the table lock, released, then published after
  CNode install + derivation record; two concurrent holds can claim the same
  index and the second publish silently overwrites the first (leaked CNode
  slot + live_holds skew).
  WHY: claim and publish are not atomic, and nothing re-validates freeness
  under the second lock take. Unreachable in Stage A (no callers); Stage B
  must re-check occupancy under the publish lock or hold the reservation.
- [S3] src/kernel/proc/proc_tcb_caps.cpp:release_on_death (raw CNode* across
  unlock) — the holder's `CNode*` is captured under the table lock and
  `remove_if` runs after unlock; a concurrent holder teardown on another CPU
  could free it first (gen check then reads freed memory).
  WHY: no ref is held across the collect-then-act gap. Unreachable in Stage
  A (table permanently empty, fast-path never takes the lock); Stage B
  should re-validate holder liveness or pin the wrapper.
- [S3] aarch64/riscv64 link evidence — production side is arch-clean (no
  arch branches, no new syscall numbers, arch-neutral wire assert), x86_64
  gates green, blocker located in a read-only test file; the #596
  pristine-base proof could not be corroborated from this session (`gh`
  empty response, recorded as limitation not evidence).
  WHY: single-arch-only gate evidence for arch-visible paths would otherwise
  gate; here nothing arch-visible changed, so this stays a note.

DECISION: APPROVED
