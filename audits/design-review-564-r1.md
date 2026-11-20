# AUDIT REPORT design-review-564-r1 (SIL 3 design audit, evidentiary standard)
PLAN: audits/plan-564.md (rev 1, base main tip 2275e89c8 — verified via git log)
FILES: plan-564.md only (design audit; no code patch; nothing committed)

BASE: `2275e89c8 #589: normalize isr_nesting baseline at kernel handover` confirmed at audit time.

## Evidence verified (read, not assumed)

- Handler homes/numbers: `sys_fork` (:44), `sys_waitpid` (:120), `sys_exec` (:393), `sys_kill` (:463), `sys_signal` (:518), `sys_sigreturn` (:596) all in `syscall_handlers_process.cpp`; `sys_getpid` in `syscall_handlers_misc.cpp:86`. Syscall numbers EXEC=20, FORK=21, WAITPID=22, GETPID=23, KILL=24, SIGNAL=31, SIGRETURN=32 (`syscall.hpp:63-75`); REPLY_RECV=97 live, MAX_SYSCALL=103 (`syscall.hpp:140,146`).
- F1/F2/F4/F5 landed: `CapType::TCB=11/VSpace=12/PageTable=13` (`cap_types.hpp:49-51`); `TcbCap::create` mints CREATOR_ONLY, kernel-only factory (`tcb_cap.cpp:77-86`); `create_supervised` is the only SUPERVISOR mint (`tcb_cap.cpp:89-102`).
- Q9 gate: `tcb_control_authorize` branches — self (SELF) / kernel-task / idle / protected-role / parent_id==caller.id CREATOR branch (no cap consulted, `tcb_control.cpp:139-145`) / SUPERVISOR-scope+grantee+target binding branch (`tcb_control.cpp:156-165`). Tripwire `kPdRoleCount==13` + `PROCFS_SRV+1==kPdRoleCount` (`tcb_control.cpp:36-43`) real.
- SDF: 13 roles, PROCFS_SRV=12, `kPdRoleCount` derived (`boot_sdf.hpp:76-121`); PROCFS_SRV SDF row unconditional (`boot_sdf.cpp:555`), TU guarded (`procfs_srv.cpp:3`).
- Fork deep copy: `TaskControlBlock::clone` (`task.cpp:1787`), fd_table copy + vnode_ref_inc, canaries; exec via `elf::exec_into_current` (`elf.cpp:819`) + `swap_page_table` (`task.cpp:1021`).
- Death funnel: `publish_task_death` ENSUREs `scheduler_lock_depth_own()==0` (`scheduler_terminate.cpp:54`), runs under IrqGuard only; `terminate_err` releases scheduler_lock_ before publish (`:172-177`); `cleanup()` backstop also publishes (`task.cpp:2862-2868`) — hooking release inside publish covers all paths.
- F2 store locking: `DerivationTable::record/take_child/has_child` all take `g_derivation_lock` (`derivation.cpp:90,128,149`); CNode teardown routes through `forget_slot` (`cap.cpp:138,162,293,330`).
- Spec: §8.8 item 2 "fork built from retype + VSpace map; no kernel deep copy after the flip" (`microkernel-transition.md:669-670`); §8.10 SDF-row+restart-class+test-class-before-MOVE (`:687-691`); §4.3b "No carved space is ever activated at F5; M14 inherits the activation guard" (`:274-275`); TCB_RESUME=101 resumes parked tasks only (`syscall.hpp:144`), not a carve-activation surface.
- Proxy precedents: 48 B wire + `owner_id/owner_gen` + `send_sync_bounded` across all PD proxies (`*_srv.cpp`, `CONFIG_IPC_FAST_PAYLOAD_BYTES=48`); `kWaitReverifyRetryMax=4` (`syscall_handlers_process.cpp:116`); `mk_death_visible_bounded` exists (`test_mk_death.cpp:249`).
- Retrieval (check-7): 9 graphify queries + 3 vault searches with per-query dispositions recorded in plan §BASE — SATISFIED, substantive.

## FINDINGS

- [S1] plan-564.md:§INV-PROC-6,step-93,test-sketch — Q9 single-gate stop authority claim contradicts the cited gate
  WHY: `tcb_control_authorize` branch 5 grants on `target.parent_id == caller.id` with no cap consulted (`tcb_control.cpp:139-145`); Stage B fork still runs `TaskControlBlock::clone` in the caller's context, so a child's `parent_id` names the calling client, never proc-mgr — a proc-mgr-as-caller stop fails branch 5, and branch 6 requires SUPERVISOR scope which the plan forbids ("CREATOR caps only"). The held CREATOR TcbCap therefore confers zero stop power through the single gate, and the sketched `proc_mgr_stop_authority` test ("proc-mgr's CREATOR cap stops a client") cannot pass against the unchanged gate. Resolve by scoping PD verdicts to the invoking client's context (then proc-mgr never calls the gate and the test is mis-scoped), or mint SUPERVISOR holds (contradicts the S1 mitigation), or add a gate branch (contradicts "no second predicate").
- [S2] plan-564.md:§INV-PROC-5,step-6,step-45 — `release_on_death` specified as lock-free single atomic store that also drops the F2 edge; unimplementable as written, funnel lock-state inverted
  WHY: every DerivationTable mutation takes `g_derivation_lock` (`derivation.cpp:90,128`), and CNode slot teardown routes through `forget_slot` (`cap.cpp:138,162,293,330`) — an edge-dropping release cannot be a bare atomic store; further, `publish_task_death` ENSUREs `scheduler_lock_depth_own()==0` (`scheduler_terminate.cpp:54`) and runs under IrqGuard only, never "under scheduler_lock_" as the plan states twice. Restate the real lock set (derivation leaf + CNode leaf under IrqGuard, no scheduler lock per M1 scope discipline), prove no order inversion vs concurrent `hold()`/revoke paths, and require idempotence on double-publish (prompt funnel + `cleanup()` backstop, `task.cpp:2862-2868`); generation-mismatch no-op already covers pid reuse.
- [S2] plan-564.md:step-17,§8.8 — Stage B debug-default flip ships a spec-violating interim state against §8.8 item 2
  WHY: item 2 requires "no kernel deep copy after the flip" (`microkernel-transition.md:669-670`), but Stage B keeps `clone_kernel_pml4` deep copy (`task.cpp:1787`) and flips the debug default to 1 while amending only §8.8 State/Protocol, leaving item 2 (Invariants) untouched. Either amend item 2 at Stage B with an explicit interim waiver pointing at F11a, or hold the default at 0 until Stage C.
- [S2] plan-564.md:§INV-PROC-7,step-1,step-8,step-15/16 — OFF-build "byte-identical" overclaimed; §8.10 ordering not literal
  WHY: Stage A unconditionally (knob-OFF) adds `PdRole::PROC_MGR`, re-derives `kPdRoleCount` 13→14, extends both allowlists, bumps both INV-ADM-15(c) asserts, and interposes `ProcSvc` delegation in all seven handlers — the binary necessarily differs. The provable claim is behavior-identity for existing roles (matches the PROCFS_SRV precedent: unconditional SDF row, guarded TU). Downgrade INV-PROC-7 accordingly and order the taskdefs restart row before the dispatch MOVE within Stage B so §8.10 ("row + restart class before MOVE rows") holds literally.
- [S3] plan-564.md:step-7,step-45 — funnel lock-state wording ("may hold scheduler_lock_")
  WHY: contradicts `scheduler_terminate.cpp:54,172-177` (ENSUREs depth 0, lock released before publish). Wording fix; substance folded into the S2 above.
- [S3] plan-564.md:step-90 — hold-failure child-destruction order vs death publication unstated
  WHY: `hold()` runs after `Scheduler::add_task_err`; the CAP_TABLE_FULL path must state whether the child is destroyed pre-publication (no death record, parent sees typed refusal) or post-publication (death-visible, waiter wake) — else a published-then-destroyed 33rd child is ambiguous to waiters.
- [S3] plan-564.md:step-96 — `getpid` O(1) "proc-mgr-served table" has no named writer
  WHY: a kernel replica of PD state risks divergence from PD ground truth; name the update source (fork/exec-path writes) or read it from the same cells the PD decision path maintains.

## Sound (no finding)

- Stage split: A preserves behavior (modulo S2 wording), B moves policy with mechanism pinned in caller context, C honestly gated — Q1(a) ship-A/B-defer-carve-to-F11a is the correct call: TCB_RESUME resumes parked tasks only, §4.3b activation guard stands, and foreign-frame-map + carved-root-activation surfaces genuinely do not exist, so carve without them would violate INV-VS-3.
- INV-PROC-1 caller-context mechanism (no trap frame/TCB pointer/stack pointer crosses; PD returns verdict only) matches verified handler homes and the clone/exec blockade structure.
- Death-path coverage (all TERMINATED paths funnel through `publish_task_death`, prompt + backstop) and (pid,gen) stale-hold closure are correctly placed; pid_reuse_safe + tracker-delta-0 acceptance is the right test.
- Q2(a) 32-cap fail-closed with typed refusal is explicit and honest; Q3 (#556 non-blocking on live REPLY_RECV/MAX_SYSCALL==103/`send_sync_bounded`) is evidence-backed.
- INV-PROC-10 arch parity (no new syscall number, arch-neutral new TUs, FPU/restore paths untouched) holds against the arch tree (fxsave/sp_el0/sepc present, no new arch branches introduced).
- Check-7 retrieval real (9 graphify + 3 vault-0-match with dispositions); S-class coverage (6×S1/5×S2/2×S3) + QE sketch (6 tests, 8 classes named, 3-arch + style + history-row gates) complete; no false-close (F11 row stays `new` through B, Appendix A deferred, carve explicitly unproven).

DECISION: REJECTED
