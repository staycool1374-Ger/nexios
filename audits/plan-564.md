# PLAN: #564 / F11 — proc-mgr PD: fork/exec/waitpid/kill + getpid/signal/sigreturn over TCB mechanisms

## REVISION HISTORY

| Rev | Date | Note |
|---|---|---|
| 1 | 2026-11-20 | Initial plan. Tip `main` 2275e89c8. Scope split into Stage A (mechanism + TCB-cap ownership), Stage B (PD + MOVE of 20-24/31-32), Stage C (carve-composed fork) — Stage C is GATED on OPEN QUESTION Q1. |
| 2 | 2026-11-20 | r1 audit fixes: INV-PROC-6 rescoped (PD never calls the Q9 gate; stop enforced in caller context), INV-PROC-5/steps-6/45 restated with real lock set (derivation+CNode leaves under IrqGuard, idempotent), Stage B carries explicit §8.8-item-2 interim waiver (F11a), INV-PROC-7 downgraded to behavior-identity + §8.10 literal ordering, funnel wording + hold-failure order + getpid writer named (S3s). |
| 3 | 2026-11-20 | r2 editorial S3s (no normative change): risk-table "byte-identical" → behavior-identity adjective (OBS-1); steps 15/16 renumbered so the restart row sorts before the dispatch MOVE it must precede (OBS-2); "byte-identical bodies" → statement-identical (source-move purity vs disclaimed binary identity, OBS-3). |

## BASE / EVIDENCE

### Retrieval (audit check-7 artifacts; dispositions)

- graphify `query_graph` "proc-mgr process manager PD fork exec waitpid kill TCB capability" (BFS d=3, 351 nodes) — USED: anchors the move surface (`fork()`, `waitpid()`, `exec_into_current()`, `TaskControlBlock`, `daemon_mgr.cpp`, `procfs.cpp`); IGNORED: PCI/VMM/virtio noise.
- graphify `query_graph` "fork clone address space deep copy VSpace retype" (122 nodes) — USED: `retype()`/`VSpaceMap`/`CarvedObjectCap`/`DerivationTable`/`clone_kernel_pml4` form the composition vocabulary; establishes that today fork composes through `clone_kernel_pml4` (kernel deep copy), not through retype.
- graphify `shortest_path` `fork()` -> `retype()` (6 hops, via `syscall.h`/`resolve_watchdog_target`) — DISPOSITION: no direct fork->retype edge exists in the tree; the composition link is exactly what F11 would have to introduce. This is the primary evidence for OPEN QUESTION Q1.
- graphify `shortest_path` `waitpid()` -> `TaskControlBlock` (5 hops via `cpu_context.hpp`) — USED: waitpid resolves the caller's children from caller context; supports INV-PROC-1 (mechanism stays in caller context).
- graphify `get_neighbors` `VSpaceMap` / `DerivationTable` and `get_node` `kill_declined()` — USED: VSpaceMap holds NO cap pins (pointer-free registry), DerivationTable is the F2 edge store; the new TCB-cap hold must therefore use F2 derivations, not VSpaceMap-style tables.
- graphify `query_graph` "how is a userspace server PD built: sdf_row sdf_compose boot_sdf role registration" — USED: `boot_sdf.cpp`/`TaskDef`/`vfs-srv.c` give the PD template; `stage_initrd.py` + `packaging/meson.build` (read) confirm userspace/*.c is auto-globbed into bin/*.c.elf (no build-list edit needed).
- graphify `query_graph` "syscall fork exec waitpid kill getpid signal sigreturn handlers" — USED: `Syscall`/`syscall_task` dispatch shape; confirms handlers live in `syscall_handlers_process.cpp` + `syscall_handlers_misc.cpp` (getpid).
- graphify `query_graph` "reply token reply_wait REPLY_RECV generation tagged call matching ipc" — USED: `reply_recv_err`, `deliver_with_reply_rule`, `try_direct_switch`, `wake_reply_waiters_of` present; syscall 97 REPLY_RECV is live and MAX_SYSCALL == 103 (`syscall.hpp:146`). DISPOSITION: F11 does NOT block on #556 (see Q3).
- graphify `query_graph` "sdf_role_bounds PdRole row count proc_mgr capacity" — USED: `PdRole`/`role_index()`/`CSlot` for the capacity analysis (the `kPdRoleCount == 13` tripwire is real: `tcb_control.cpp:37,42`).
- obsidian (vault `jarvis`): searches "proc-mgr process manager F11", "fork exec waitpid process manager spec", "pid reuse death visible supervisor" -> 0 matches each. DISPOSITION: no vault coverage for F11; no prior audit findings to apply; recorded so the next cycle need not re-hunt (matches the work-begun comment on #564).

### Verified tree facts at 2275e89c8 (read, not assumed)

- F1/F2/F4/F5 landed: `CapType` TCB/VSpace/PageTable/... (`cap/cap_types.hpp`); `TcbCap` with CREATOR_ONLY / SUPERVISOR scope (`cap/tcb_cap.hpp`); `TcbCap::create()` has NO syscall producer today (kernel-only factory, `tcb_cap.cpp:77,89`); `tcb_control` is the single Q9 authority gate (`task/tcb_control.hpp`); `VSpaceMap` link/teardown registry (`cap/vspace_map.hpp`); MAX_SYSCALL 103.
- 20-24/31-32 are live and NOT PD-routed: `sys_fork` (`syscall_handlers_process.cpp:44`), `sys_waitpid` (:120), `sys_exec` (:393), `sys_kill` (:463), `sys_signal` (:518), `sys_sigreturn` (:596), `sys_getpid` (`syscall_handlers_misc.cpp:86`).
- fork today = `TaskControlBlock::clone(regs)` (`task.cpp:1787`): TCB alloc, kslot kernel stack, PMM user stack, `clone_kernel_pml4`, fd_table copy + vnode_ref_inc, cwd, FPU save per arch, canaries. This is the deep copy §8.8 wants gone "after the flip".
- exec today = `elf::exec_into_current` (`elf.cpp:819`): builds a fresh pml4 and swaps via `swap_page_table` (:977). Mechanism already produces a fresh VSpace; it is caller-bound.
- SDF: `PdRole` has 13 roles (PROCFS_SRV = 12), `kPdRoleCount` = 13 (`boot_sdf.hpp:76-121`); `sdf_check_bounds` is the INV-MK-2 compose gate; `sdf_policy_only`/`sdf_zero_caps_allowed` are the policy-PD allowlists.
- The two build tripwires: `boot_sdf.hpp:120` (`kPdRoleCount`) and `tcb_control.cpp:37,42` (`PdRole::PROCFS_SRV + 1 == kPdRoleCount`, `kPdRoleCount == 13`, INV-ADM-15(c)). Adding a role MUST pay this revisit.
- `stage_initrd.py` takes `--root`/`--bin`; `packaging/meson.build:86-145` auto-globs `userspace/*.c` into bin targets, so a new PD binary needs no build-list edit. `src/meson.build` globs kernel sources with `find` (reconfigure picks up a new .cpp).
- `mk_death_visible_bounded` ALREADY EXISTS (`test/test_mk_death.cpp:249`, M1). F11 must not duplicate it.

## AFFECTED FILES

- `src/kernel/nexios_config.h` — add `CONFIG_PROC_MGR` (default 0, procfs-srv style master knob), `CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS`, `CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS` (32, fail-closed ceiling). OFF build must stay behavior-identical for existing roles (INV-PROC-7).
- `src/kernel/proc/proc_errors.hpp` (new) — X-macro `ProcError`: OK, NO_SERVER, RPC_BOUND, STALE_PID, CAP_TABLE_FULL, HOLD_INSTALL_FAILED, BAD_REQUEST, STOP_REFUSED. Module error header per CODING_STYLE §5.3 (never a bare -1).
- `src/kernel/proc/proc_wire.hpp` (new) — `enum class ProcMgrOp` (FORK/EXEC/WAITPID/KILL/GETPID/SIGNAL/SIGRETURN/STOP/RESUME/BIND/LOOPBACK) and the 48 B `ProcMgrRpc` wire struct (`static_assert(sizeof == 48)`, pointer-free, `owner_id`/`owner_gen` per the #441 dead-letter lesson).
- `src/kernel/proc/proc_svc.hpp/.cpp` (new) — service layer owning the CURRENT handler bodies: `ProcSvc::fork/exec/waitpid/getpid/kill/signal/sigreturn` (pure move from `syscall_handlers_process.cpp`, statement-identical bodies) plus `ProcSvc::handle_decision(const ProcMgrRpc&)` for the PD's verdict. No new policy here in Stage A.
- `src/kernel/proc/proc_tcb_caps.hpp/.cpp` (new) — the §8.8 protocol table: `hold(holder, target_id, target_gen)`, `lookup(holder, pid)`, `release(holder, pid, gen)`, `release_on_death(pid, gen)` (atomic, callable from the death funnel), `snapshot_reset()`. Fixed array, leaf SpinLock for the RPC path, lock-free atomic clear for the death path.
- `src/kernel/proc/proc_mgr_proxy.hpp/.cpp` (new) — PID cell, epoch, liveness, bounded Call to proc-mgr, typed fallbacks (procfs_srv.cpp shape), everything inside `#if CONFIG_PROC_MGR`.
- `src/kernel/syscall/syscall_handlers_process.cpp` — handlers become thin: keep the exact signatures, FAST/FULL classification, user-pointer validation and caller-context mechanism; delegate the body to `proc::ProcSvc::*` and, when `CONFIG_PROC_MGR`, take the PD decision via the proxy first (INV-PROC-1).
- `src/kernel/syscall/syscall_handlers_misc.cpp` — `sys_getpid` returns the identity from the proc-mgr-served table when the knob is on (O(1) read, no RPC); OFF build unchanged.
- `src/kernel/task/scheduler_lifecycle.cpp` — in `Scheduler::publish_task_death` (M1 funnel, IrqGuard only, scheduler_lock_ NOT held per scheduler_terminate.cpp:54,172-177), after the record latch: one call to `proc::proc_tcb_caps::release_on_death(pid, gen)` (derivation + CNode leaves, idempotent, no logging).
- `src/kernel/task/boot_sdf.hpp/.cpp` — `PdRole::PROC_MGR = 13`; `kPdRoleCount` = 14; a zero-device row (policy PD); `sdf_role_bounds(PROC_MGR)` (object_budget = default 32, untyped_quota 0, death_watches 0 — the hold releases from the funnel, not a watch); `sdf_policy_only` and `sdf_zero_caps_allowed` gain PROC_MGR.
- `src/kernel/task/tcb_control.cpp` — bump both INV-ADM-15(c) `static_assert`s to 14 with the revisit note: proc-mgr is NOT mem-mgr, the exclusion list is unchanged, and proc-mgr's stop authority is the CREATOR branch (Q9 client subtree), not SUPERVISOR scope.
- `src/kernel/task/taskdefs.cpp/.hpp` — `proc-mgr` row (TaskType::SPORADIC_SERVER, elf `bin/proc-mgr.c.elf`, sdf_role PROC_MGR, `on_restart_fn` = `pd_rebind_role(PROC_MGR)`, pid cells) guarded by `#if CONFIG_PROC_MGR`; widen the `d.sdf_role > PROCFS_SRV` validation bound to PROC_MGR.
- `src/kernel/log/dmesg_catalog.hpp` + push sites — 2-3 new TASK-base codes (proc-mgr compose/restart/hold-full), chosen after a grep of live offsets (LEARNINGS #354: +11/+12 collisions).
- `userspace/proc-mgr.c` (new) — the PD: BIND/LOOPBACK + the seven ops; replies to `owner_id`/`owner_gen` (never PID 1, #441); denies malformed input with a typed status (R-INPUT).
- `userspace/proc-mgr-parity.c` (new) — parity probe used by the parity test class (driver-pd-parity/timer-srv-parity shape).
- `docs/specs/microkernel-transition.md` — §8.8 State/Protocol/Verification + §8.8 item 2 INTERIM waiver ("Stage B retains clone deep copy; carve fork deferred to F11a") + §15 F11 row + Appendix A record once Stage B lands; §4.3b stays as-is unless Q1 selects Stage C.
- TEST-ONLY (testbed lane, NOT this dev branch): `src/kernel/test/test_registry.cpp` (`proc_mgr` class), `src/kernel/test/test_proc_mgr.cpp`, `src/kernel/test/test_expected_counts.hpp`.

## INVARIANTS & CONCURRENCY BOUNDARIES

- INV-PROC-1 (caller context): every process mechanism (clone, elf load/swap, waitpid block, terminate, trap-frame restore) runs in the CALLING task's context with the caller's `regs`. No trap frame, TCB pointer or kernel stack pointer ever crosses the PD boundary; the PD returns a verdict only.
- INV-PROC-2 (bounded Call): every RPC to proc-mgr goes through the bounded `IPC::send_sync` form with `CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS` (INV-MK-3); no unbounded wait, no polling loop.
- INV-PROC-3 (no lock across RPC): the handler releases every lock (CNode leaf, proc_tcb_caps leaf) BEFORE `send_sync`. Lock order stays: scheduler_lock_ -> frame/box registries -> leaf; proc_tcb_caps is a leaf taken alone (resolve, capture, release, then Call). Never hold scheduler_lock_ across the RPC (deferred-switch arm, CODING_STYLE §11.1).
- INV-PROC-4 (bounded hold table): `CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS` = 32 fixed slots, no allocation on the fork path. Overflow = typed `ProcError::CAP_TABLE_FULL` with the child fully destroyed (no zombie, no half-hold), never a panic.
- INV-PROC-5 (generation-matched release): a hold is keyed (pid, generation). `release_on_death` runs in the M1 death funnel under IrqGuard with `scheduler_lock_depth_own()==0` (scheduler_terminate.cpp:54; terminate_err releases scheduler_lock_ before publish :172-177) and takes the derivation leaf (`g_derivation_lock`) + the holder-CNode leaf in leaf order — never scheduler_lock_ (M1 scope discipline). It is idempotent across double-publish (prompt funnel + `cleanup()` backstop task.cpp:2862-2868): second call is a generation-mismatch no-op. No order inversion vs concurrent `hold()`/revoke: both take derivation leaf -> CNode leaf, never the reverse; the funnel path takes the same order under IrqGuard. A recycled pid can never match a stale hold (generation equality required).
- INV-PROC-6 (Q9 reuse, one gate, PD never calls it): `tcb_control_authorize` stays the single stop gate unchanged. The PD NEVER calls the gate as caller: branch 5 grants on `target.parent_id == caller.id` with no cap consulted (tcb_control.cpp:139-145), and Stage B fork still clones in the invoking client's context, so a child's parent_id names the client, never proc-mgr — a proc-mgr-as-caller stop would fail branch 5, and branch 6 needs SUPERVISOR scope which the plan forbids. Instead every stop is enforced in the INVOKING CLIENT's context: the proxy supplies the disposition verdict, the kernel mechanism calls the gate as the client, and the existing parent_id branch decides. The held CREATOR TcbCaps are lifecycle-tracking holds (§8.8 protocol: proc-mgr holds a cap for every task it created), not stop power. No second predicate is written; SUPERVISOR scope stays kernel-compose-only (tcb_cap.hpp:29).
- INV-PROC-7 (OFF-build behavior identity): with `CONFIG_PROC_MGR=0` the kernel behaves identically to today for all existing roles (procfs-srv precedent: unconditional SDF row, guarded TU). The binary necessarily differs (new PdRole value, re-derived kPdRoleCount, extended allowlists, bumped asserts, ProcSvc delegation wrappers) — the provable claim is behavior-identity, not byte-identity.
- INV-PROC-8 (ResourceTracker): TcbCap folds into the existing `cap_objects` counter (F4); no new resource class. Every test path (fork, kill, restart, hold-full) must end at delta 0; `proc_tcb_caps::snapshot_reset()` is wired into test isolate restore.
- INV-PROC-9 (SDF accounting): the proc-mgr row declares 0 device caps, 0 Untyped quota, 0 death watches, object_budget 32 — `sdf_check_bounds` must accept it and `mk_inv_static_bounds`-class demand checks must remain green.
- INV-PROC-10 (arch parity): no new syscall number is introduced, so the dispatch table is untouched; the 48 B wire `static_assert` and every new TU are arch-neutral — no `#if defined(CONFIG_ARCH_*)` branches. Fork/exec arch work is confined to the existing FPU save (`arch::fxsave` vs `arch::fpu_save`) and context restore paths, which are unchanged.

## STEP-BY-STEP CHANGES

### Stage A — mechanism + TCB-cap ownership (CONFIG_PROC_MGR=0, no syscall moved)

1. `src/kernel/nexios_config.h`: add `CONFIG_PROC_MGR` (0), `CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS`, `CONFIG_PROC_MGR_MAX_HELD_TCB_CAPS` (32) next to the `CONFIG_PROCFS_SRV` block; each with the "1 = ... , 0 = shipped default" doc comment.
2. `src/kernel/proc/proc_errors.hpp`: X-macro enum + `error_string<ProcError>()`, values named per the module-error rule; no reuse of `SYS_ERR_GENERIC_FAIL` internally (handlers still translate to it at the boundary).
3. `src/kernel/proc/proc_wire.hpp`: `ProcMgrOp` + `ProcMgrRpc` (op, flags, epoch, owner_id, owner_gen, seq, op-scoped `arg0..arg2`, status) with `static_assert(sizeof(ProcMgrRpc) <= CONFIG_IPC_FAST_PAYLOAD_BYTES)` and an aligned 48 B layout.
4. `src/kernel/proc/proc_svc.cpp`: move the bodies of `sys_fork` (:44-61), `sys_waitpid` (:120-319), `sys_exec` (:393-461), `sys_kill` (:463-516), `sys_signal` (:518-594), `sys_sigreturn` (:596-620) and `sys_getpid` verbatim into `ProcSvc::*` as free functions taking the same parameters (`uint64_t* regs` included). Statement-identity criterion: `git diff -U0` shows pure addition/removal of wrappers, no statement edits; the #590 publication/revive-retry logic and the #593 trap-funnel kill leg move unchanged.
5. `src/kernel/syscall/syscall_handlers_process.cpp` + `syscall_handlers_misc.cpp`: each `Syscall::sys_*` keeps its signature and its early gates (user-task check, path/argv validation), then `return proc::ProcSvc::fork(regs);` etc. No behaviour change with the knob OFF.
6. `src/kernel/proc/proc_tcb_caps.cpp`: fixed `Slot { pid, gen, slot, cap_gen, state }` array; `hold()` does `ensure_cspace(holder)`, `TcbCap::create(pid, gen)`, installs into the holder's CNode, records the F2 derivation edge (parent = holder's own creator cap where one exists, else a fresh root edge), and returns the slot; `lookup()` resolves handle -> `TcbCap` -> tag/generation check (thread_cap shape) and returns the live target; `release_on_death()` takes derivation leaf + holder-CNode leaf under IrqGuard (same order as `hold()`), clears the slot, drops the edge, idempotent on double-publish (generation-mismatch no-op); `snapshot_reset()` clears all slots.
7. `src/kernel/task/scheduler_lifecycle.cpp`: in `publish_task_death`, immediately after the record latch and the daemon-cell clear, add `proc::proc_tcb_caps::release_on_death(dead.id, dead.generation)` — derivation + CNode leaves under IrqGuard (scheduler_lock_ NOT held here), idempotent, no logging (no logging under registry locks).
8. `src/kernel/task/boot_sdf.hpp`: add `PROC_MGR = 13` to `PdRole`, re-derive `kPdRoleCount`, extend `kPdRoleCount`'s comment (F11 #564); `boot_sdf.cpp`: `sdf_row(PROC_MGR)` returns 0 caps; `sdf_role_bounds(PROC_MGR)` returns the defaults (max_caps = kMaxCapsPerRole, object_budget = CONFIG_MAX_PER_TASK_OBJECTS, quotas 0); add PROC_MGR to `sdf_policy_only` and `sdf_zero_caps_allowed`.
9. `src/kernel/task/tcb_control.cpp`: both `static_assert`s -> 14, with the INV-ADM-15(c) revisit comment: proc-mgr is not the mem-mgr, is not SUPERVISOR-scoped, and adds no exclusion.
10. `src/kernel/proc/proc_mgr_proxy.cpp`: `set_pid`/`get_pid` cell with generation validation (dead-cell self-heal, daemon_mgr pattern), `epoch` counter, `ensure_running()`, and `call(const ProcMgrRpc&, ProcMgrRpc& out)` = bounded `IPC::send_sync` with typed NO_SERVER / RPC_BOUND. Entire TU inside `#if CONFIG_PROC_MGR`.
11. `src/kernel/log/dmesg_catalog.hpp` + push sites: add rows for (a) proc-mgr compose/rebind failure, (b) hold-table full, (c) RPC bound expiry; pick the next free `kDmesgBase_TASK` offsets after grepping live users.
12. Docs: §8.8 Protocol gains the implementation note "TCB cap hold + funnel release", §15 F11 row keeps `new`, Appendix A entry deferred to Stage B.

### Stage B — PD + MOVE of 20-24 / 31-32 (CONFIG_PROC_MGR=1 path)

13. `userspace/proc-mgr.c`: sporadic-server main loop (procfs-srv.c shape) handling BIND/LOOPBACK + the seven ops; validates op/flags/rsvd/owner fields first (R-INPUT), replies to `owner_id`/`owner_gen`, never to a fixed pid; unknown op -> typed status, no exit.
14. `userspace/proc-mgr-parity.c`: parity probe that issues each op against the kernel path and the PD path and reports mismatches through the standard parity channel.
15. `taskdefs.cpp`: proc-mgr row FIRST (priority 4, period 10, 1/1 sporadic, `bin/proc-mgr.c.elf`, sdf_role PROC_MGR, rebind + pid-cell hooks), guarded `#if CONFIG_PROC_MGR`; widen the `sdf_role` validation bound. Ordering is literal per §8.10: SDF row (step 8) + restart row (this step) land BEFORE the dispatch MOVE (step 16) takes effect.
16. Kernel dispatch (knob ON only), per op, all in caller context:
    - `fork(21)`: proxy decision (quota/ownership) -> on OK, `TaskControlBlock::clone(regs)` + `Scheduler::add_task_err` exactly as today -> `proc_tcb_caps::hold(proc_mgr_tcb, child->id, child->generation)` -> return child id. Hold failure (CAP_TABLE_FULL) destroys the child PRE-publication: the child never reaches `publish_task_death`, no death record is latched, waiters see only the typed refusal (never a death-visible 33rd child).
    - `exec(20)`: proxy verdict (path policy) -> existing validate/read/`elf::exec_into_current` unchanged; the task id is preserved, the hold is untouched.
    - `waitpid(22)`: proxy supplies the ownership predicate ("is pid my child"); the blocking/reap machinery (#590 publication + revive-retry, `kWaitReverifyRetryMax`) stays statement-identical in caller context.
    - `kill(24)`: proxy supplies the disposition class (fatal vs handler); delivery stays `pending_signals` + the #593 trap-funnel leg in the invoking client's context; the gate is called as the client (existing parent_id branch decides). A PD verdict can never stop a NON-child because the mechanism enforces it as the client — never a proc-mgr-as-caller gate call (INV-PROC-6).
    - `signal(31)`: proxy supplies the handler-table decision; installation stays per-TCB.
    - `sigreturn(32)`: mechanism only (trap-frame restore); the proxy is not on this path — documented as a policy/mechanism split (the handler table it restores is proc-mgr-owned via `signal`).
    - `getpid(23)`: O(1) identity read; writer is the fork/exec path (same cells the PD decision path maintains — no PD/thunk replica to diverge). No RPC on the hot path (bounded by construction, INV-PROC-2 satisfied trivially).
17. Flip the knob default to 1 for the debug build only after the QE parity class is green AND the §8.8 interim waiver above is in the tree; release default stays 0 until the 2x3 matrix is re-run (per-server gate §7 of the migration spec).

### Stage C — carve-composed fork (GATED: do NOT implement until Q1 is answered)

18. Design-only at this plan's rev: `retype(Untyped -> TCB)`, `retype(-> VSpace root)`, N x `retype(-> PageTable)`, `VSPACE_MAP` top-down link, frame retype + a NEW frame-into-foreign-VSpace map surface, a NEW activation surface (bind/activate the carved VSpace root on the TCB) and the §4.3b activation-guard amendment. Stage C is listed here so the invariant of §8.8 item 2 is not silently dropped, but it is NOT executable without Q1.

## TEST STRATEGY

Test sanctity: this dev lane lands NO tests on main. The list below is handed to QE as a stub-first sketch for the testbed branch (`JARVIS_TEST_PASS()` stubs + `test_expected_counts.hpp` row first).

- New tests (testbed lane):
  - `proc_mgr_parity` (class `proc_elf`) — for each of fork/exec/waitpid/getpid/kill/signal/sigreturn: identical return values and identical observable TCB/VSpace state on the kernel path vs the `CONFIG_PROC_MGR` path; pseudo-code: fork -> assert child id, parent_id, page_table_ != parent's; kill -> assert pending_signals bit; waitpid -> assert exit code and reaped state.
  - `pid_reuse_safe` (class `proc_elf`) — kill + reap a child, allocate a new task, assert `proc_tcb_caps::lookup` by (pid, gen) fails typed, that a stale handle resolve refuses on generation mismatch, and that no cap_objects/PMM delta survives (tracker delta 0).
  - `proc_mgr_crash_restart` (class `servers`) — kill the proc-mgr PD; assert supervisor restart via `sdf_rebind` + epoch bump, that in-flight clients get a typed error (not a hang, #560 path), that held TCB caps are re-minted for live children only, tracker delta 0.
  - `proc_mgr_stop_authority` (class `cap_tcb`) — stop authority stays in caller context (INV-PROC-6): a CLIENT caller stops its own child via the unchanged parent_id branch; refusals for self, kernel task, idle, mem-mgr (`tcb_control_role_protected`), and a cross-subtree client caller, each typed and side-effect-free; the PD-as-caller path is explicitly NOT tested (it cannot pass the unchanged gate by construction). Complements the existing `mk_supervisor_stop`.
  - `proc_mgr_hold_full` (class `cap_tcb`) — the 33rd hold fails `CAP_TABLE_FULL`, the 33rd child is destroyed with no zombie and no leak; no panic.
  - `proc_mgr_death_visible` (class `proc_elf`) — a PD-originated fault kill is reported through the M1 death record within the Q4 bound; must CALL the existing `mk_death_visible_bounded` helpers, not duplicate them.
- Existing classes to run (rationale in line):
  - `proc_elf` (135/81 expected) — the direct blast radius: process lifecycle, ELF load, pml4 clone, pt_merge, libc syscall wrappers.
  - `core_task` — TCB clone/teardown/funnel paths, the #409 teardown claim and death record.
  - `capability` — includes the cap_tcb, cap_vspace, cap_broker and sdf_bounds groups; catches the kPdRoleCount bump and the SDF demand check.
  - `cap_tcb` / `cap_vspace` — the F4/F5 surfaces the new hold reuses.
  - `ipc` — bounded Call semantics used by the proxy (INV-PROC-2).
  - `servers` — SDF compose/rebind/restart class for the new row.
  - `selftest` — CI gate class.
- Expected-counts: `test_expected_counts.hpp` gains a `proc_mgr` row and `proc_elf`/`cap_tcb`/`servers` rows are bumped by the number of added tests (compile-time checked sum).
- Gates for this change (evidence before any audit request): `tools/meson/build.sh --arch {x86_64,aarch64,riscv64}` all three green (INV-PROC-10); `make check-style` Errors 0; the classes above on x86_64 debug; then the aarch64/riscv64 legs of the same classes; one `test-history.txt` row per class run (append-only, measured times only).
- Flake discipline: the H2-family TIMEOUT/PANIC flake is documented on main; any zero-S-line stall gets one retry and a STATUS row, and is never recorded as PASS.

## SIL 3 / SAFETY RISKS

- [S1] Fork/exec moved behind an RPC could change ordering or resume a different task's context — mitigation: INV-PROC-1 (mechanism in caller context, verdict-only replies), INV-PROC-7 (OFF-build behavior-identity), `proc_mgr_parity` for every op, and a `git diff -U0` review proving the moved bodies are statement-identical.
- [S1] TCB-cap hold leak on any fork/teardown path (double hold, missed release, restart) — mitigation: bounded fixed table (INV-PROC-4), generation-matched atomic release at the M1 funnel (INV-PROC-5), `pid_reuse_safe` + tracker delta 0 on every path, `snapshot_reset()` in isolate restore.
- [S1] Stale hold resolving after pid/generation reuse (authority leak: stopping the wrong task) — mitigation: (pid, gen) key on both hold and lookup, generation equality in the resolve, `pid_reuse_safe` as the acceptance test.
- [S1] Deadlock: taking the hold lock or scheduler_lock_ across the blocking Call — mitigation: INV-PROC-3 (resolve -> capture -> release -> Call), leaf-only lock, no logging under registry locks, and a code-read of every call site in review.
- [S1] Q9 stop authority widened by a second predicate or by the PD holding SUPERVISOR-scope caps — mitigation: INV-PROC-6 (PD never calls the gate; stop enforced in invoking-client context via the unchanged parent_id branch; held CREATOR caps are lifecycle tracking only; SUPERVISOR scope remains kernel-compose-only per `tcb_cap.hpp:29`), `proc_mgr_stop_authority` refusals via client callers.
- [S1] proc-mgr death while holding caps leaves orphaned authority and hung clients — mitigation: restart = revoke derivations + `sdf_rebind` + epoch bump (§8.7/§10 D4), typed errors to waiters via the #560 path, `proc_mgr_crash_restart`.
- [S2] The `kPdRoleCount == 13` tripwire is bumped — this is the intended INV-ADM-15(c) revisit; mitigation: the static_assert text names F11 and the reviewed conclusion (not mem-mgr, no new exclusion), plus a comment at both sites.
- [S2] CSlot/object budget: 32 held TCB caps vs `CONFIG_MAX_TASKS` 64 — mitigation: fail closed at 32 with a typed error (no panic, no partial child) and an owner question (Q2) rather than a silent overrun of `CONFIG_MAX_PER_TASK_OBJECTS`.
- [S2] Wire-struct drift vs `CONFIG_IPC_FAST_PAYLOAD_BYTES` (48) — mitigation: `static_assert` on size and alignment, per-op field audit, and the `BlockRpc` precedent.
- [S2] dmesg catalog collision for the new codes — mitigation: grep live offsets before choosing (LEARNINGS #354), one row per new code.
- [S3] Parity probe cost in release — mitigation: probe compiled only under `CONFIG_PROC_MGR` + the parity knob, OFF by default in release.
- [S3] proc-mgr restart class ambiguity — resolved, not deferred: Q12 DECIDED critical = {supervisor, mem-mgr}; proc-mgr is `restartable`; document it in the SDF row comment so a future reader cannot re-litigate it silently.

## OPEN QUESTIONS

- Q1 (BLOCKS Stage C only — Stage A/B proceed): §8.8 item 2 requires "fork built from retype + VSpace map; no kernel deep copy after the flip", but §4.3b/INV-VS-3 says "No carved space is ever activated at F5; M14 inherits the activation guard", and N1-N9 contains no surface to (a) map a FrameCap into a VSpace other than the caller's, or (b) activate a carved VSpace root on a TCB. Sketched options: (a) F11 ships the PD/MOVE with the kernel-composed fork and Stage C lands as a tracked follow-up (F11a) once two new numbers + the §4.3b amendment are owner-approved; (b) F11 blocks on the full carve composition and slips M14; (c) spec amendment that M14 keeps kernel composition permanently. Recommendation: (a) — it delivers the whole policy MOVE and the TCB-cap protocol now, keeps the invariant explicitly deferred rather than silently violated, and costs no new syscall numbers in this cycle. NOT VIABLE: shipping carve-composed fork without an activation surface (silently violates INV-VS-3).
- Q2 (does not block: the plan ships the fail-closed variant): Q7 sized `CONFIG_MAX_PER_TASK_OBJECTS` 256 for mem-mgr/supervisor and 32 elsewhere, and granted 2 CNodes to infra servers. proc-mgr needs one TCB cap per created task (up to `CONFIG_MAX_TASKS` 64). Options: (a) ship fail-closed at 32 with a typed refusal and file the quota extension; (b) owner extends the proc-mgr row to 256 + 2 CNodes now. Recommendation: (a) for this cycle. NOT VIABLE: silently exceeding the declared bound (INV-MK-2 violation at compose time).
- Q3 (answer recorded, no owner action needed): F11 does NOT block on #556. REPLY_RECV (97) is live, MAX_SYSCALL == 103, and bounded `IPC::send_sync` covers INV-PROC-2; the direct-switch part of F3 is a latency optimisation measured by F17 (#570), not a correctness precondition for the proc-mgr RPC.
