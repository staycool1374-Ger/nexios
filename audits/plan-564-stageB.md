# PLAN: #564 / F11 Stage B — proc-mgr PD + MOVE of syscalls 20-24/31-32 behind CONFIG_PROC_MGR

## REVISION HISTORY

| Rev | Date | Note |
|---|---|---|
| 1 | 2026-11-20 | Initial Stage B plan. Base \`main\` = \`de0a4dc3b\` (Stage A merged). Binding parent: \`audits/plan-564.md\` rev 3 steps 13-17, INV-PROC-1..10 rev 2, Q1/Q2/Q3. Scope: PD binary + parity probe + per-op verdict legs in caller context + taskdefs row + knob flip policy + docs. Stage C stays GATED (no carve surfaces, no 4.3b amendment). |

## BASE / EVIDENCE

### Base verification

- \`.git/refs/heads/main\` = \`de0a4dc3b7570eac87df54951ed814977080f20b\`; \`refs/remotes/origin/main\` identical. Matches the prompt's \`de0a4dc3b\`; Stage A is the tip.

### Retrieval (audit check-7 artifacts; queries + dispositions)

- graphify \`query_graph\` "proc-mgr PD template: procfs-srv sporadic server main loop BIND LOOPBACK reply to owner_id" (BFS d=3, 434 nodes) — USED: \`userspace/procfs-srv.c\` is the exact PD template (torn-payload \`rc != req.op\` check, \`rep.owner_id/owner_gen/status_op\` echo, \`ipc_send(req.owner_id, MSG_TYPE_REPLY, ...)\` so the reply is never a PID-1 dead letter — #441), plus \`SporadicServer\`, \`daemon_mgr.cpp\`, \`TaskDef\`. IGNORED: ReadyQueueManager/PciBdf/Vnode/pci.hpp noise (no bearing on the PD shape).
- graphify \`query_graph\` "userspace PD build packaging stage_initrd auto-glob userspace c into bin elf taskdefs TaskDef sdf_role" (BFS d=3, 549 nodes) — USED: \`taskdefs.cpp\`/\`TaskDef\`/\`reboot_from_table\`/\`ElfLoader\`; confirms the packaging glob (read: \`packaging/meson.build:86-147\` finds every \`userspace/*.c\` and links \`<name>.c.elf\` into \`bin/\`), so a new PD needs **no** build-list edit. IGNORED: ELF dyn-link/shared-lib internals (exec path unchanged).
- graphify \`query_graph\` "supervision restart pd_rebind_role daemon death funnel on_restart pid cell death_watches" (BFS d=3, 126 nodes) — USED: \`daemon_mgr.cpp:176 restart_stale_daemons\`, \`:304 on_restart_fn\`, \`:94 register_daemon_ex\`, \`pd_rebind_role\` (\`taskdefs.cpp:85\`), \`publish_task_death\`, \`DeathNotify\`. Confirms the restart hook is the only self-heal path (no \`ensure_running()\` in the proxy). IGNORED: fdt/DTB nodes.
- graphify \`shortest_path\` \`sys_fork\` → \`Scheduler::add_task\` (5 hops, via \`Syscall\`/\`handle\`/\`TEST_CLASS\`) — DISPOSITION: no direct edge; the chain runs through the dispatch table, so the Stage B hook belongs in the **moved body** (\`ProcSvc::fork\`), not in \`syscall_table_\`. Evidence that no dispatch-table edit is needed (no new syscall numbers, INV-PROC-10).
- graphify \`shortest_path\` \`proc-mgr\` → \`sdf_rebind\` (3 hops, "source match was ambiguous") — DISPOSITION: ambiguity = the graph holds no proc-mgr row yet (Stage B adds it). Used instead the real template: \`taskdefs.cpp:273-289\` (procfs-srv hooks) + \`boot_sdf.cpp:861-865\` (\`sdf_rebind\` = re-derive via \`sdf_compose\`, never inherit).
- obsidian \`obsidian_search_vault\` (vault \`jarvis\`) "proc-mgr" → 10 matches, **all** \`audits/design-review-564-r1.md\`, \`-r2.md\`, \`-code-r1.md\`, \`audits/plan-564.md\` (vault mirrors of tracked repo artifacts). USED: code-r1 findings carried into steps 2/14 (hold claim/publish S3, funnel raw-CNode S3, unwired \`snapshot_reset\`). "F11 fork exec process manager" → 0 matches; "CONFIG_PROC_MGR" → 10 matches, same artifacts. DISPOSITION: the prior cycle's "no spec coverage in the vault" still holds; the only vault evidence is the audit mirrors, which are already in-tree.

### Stage A anchor drifts (carried; verified in tree at de0a4dc3b — do not re-discover)

1. **Death funnel home.** \`Scheduler::publish_task_death\` lives in \`src/kernel/task/scheduler_terminate.cpp:47\`; the hold hook is already at \`:85\` under \`IrqGuard\` with \`scheduler_lock_\` not held (\`:172-177\` releases before publish). plan-564 rev 3 named \`scheduler_lifecycle.cpp\`. **No Stage B change** — Stage A wired the true funnel.
2. **Proxy liveness.** \`proc_mgr_alive()\` is an inline \`find_task\` + \`!= TERMINATED/REAPED\` probe; failure is typed \`PROC_ERR_NO_SERVER\`; there is no \`ensure_running()\` and no dead-cell self-heal; \`proc_mgr_on_restart(pid)\` overwrites the PID cell and bumps the epoch. Stage B adds the taskdefs restart hook that calls it (step 6).
3. **Derivation edges.** Fresh-root self-edge (\`proc_tcb_caps.cpp:183-186\`, same shape as \`untyped.cpp:351\`); no creator cap exists, so no cross-CNode parenting. Stage B keeps the root edge; Stage C owns parenting.
4. **NEW drift (found this cycle, not in the brief): forked children carry \`generation == 0\`.** \`TaskControlBlock::clone\` (\`task.cpp:1787\`) does not route through \`init_task_common\` (\`task.cpp:446\`, the only assigner of \`tcb.generation\`) and never sets the field, so every fork child is generation 0. The (pid, generation) key that INV-PROC-5 (hold release), \`resolve_live_target\` (\`proc_tcb_caps.cpp:90\`) and \`tcb_control\` (\`tcb_control.cpp:104\`) rest on is therefore **degenerate for exactly the population proc-mgr holds**. \`alloc_id()\` is monotone in production (\`scheduler_id_table.cpp:72-94\`) but snapshot restore rewinds \`next_task_id_\`, so pid reuse *is* reachable in tests. Step 1 fixes this; it is the one structural addition Stage B makes to Stage A.
5. **\`proc_tcb_caps::snapshot_reset()\` is unwired** (code-r1 V9: no caller anywhere). It is a test-file seam (\`src/kernel/test/test_isolate.cpp\`) → QE handoff, not this lane (step 14 / test strategy).
6. **\`sdf_role\` bound is already widened** to \`PdRole::PROC_MGR\` (\`taskdefs.cpp:498-504\`, Stage A), so plan-564 step 15's "widen the validation bound" is a **no-op today**; the row itself is the only taskdefs change.
7. Stage A also already landed: \`CONFIG_PROC_MGR\`=0 + 0/1 validator + numerics (\`nexios_config.h:1581-1605,1686-1693\`), \`PdRole::PROC_MGR=13\`/\`kPdRoleCount=14\`, zero-cap row (\`boot_sdf.cpp:560-566\`), \`sdf_policy_only\`/\`sdf_zero_caps_allowed\`/\`pd_owns_device\` membership, both INV-ADM-15(c) asserts at 14, dmesg \`+18/+19/+20\`, and the §8.8 item-2 INTERIM waiver text.

## AFFECTED FILES

- \`src/kernel/task/task.cpp\` — generation allocator extracted to one internal helper; \`clone()\` assigns a real generation next to its \`alloc_id()\` (drift 4). No other clone semantics change.
- \`src/kernel/proc/proc_tcb_caps.cpp\` — fix the two code-r1 S3s that become reachable the moment \`hold()\` has a caller: reserved-slot claim/publish (no more silent overwrite) and the funnel's foreign-\`CNode*\` dereference (self-drain reclaim instead). \`snapshot_reset()\` also clears the new flags.
- \`src/kernel/proc/proc_tcb_caps.hpp\` — contract text for the reserved/reclaim states and the new self-drain rule; no signature change to the five public entry points.
- \`src/kernel/proc/proc_mgr_proxy.cpp\` — add \`proc_mgr_holder()\`, the \`proc_mgr_verdict()\` request-builder + \`handle_decision\` validation, the RPC_BOUND dmesg push, and the two probe seams. Still entirely inside \`#if CONFIG_PROC_MGR\`.
- \`src/kernel/proc/proc_mgr_proxy.hpp\` — declarations for the above (guarded), with the owner/epoch/echo contract restated.
- \`src/kernel/proc/proc_svc.cpp\` — the seven knob-ON verdict legs in caller context (fork/exec/waitpid/kill/signal only; \`sigreturn\`/getpid deliberately untouched) + the fork hold/release wiring with pre-publication hold failure.
- \`src/kernel/proc/proc_svc.hpp\` — comment-only: the Stage B entry point is now live; the deny-only verdict rule is stated where the bodies live.
- \`src/kernel/task/taskdefs.cpp\` — \`#if CONFIG_PROC_MGR\` hook block (restart + pid cell) and the \`proc-mgr\` row appended after \`procfs-srv\`.
- \`userspace/proc-mgr.c\` (new) — the PD: BIND/LOOPBACK + the seven ops, R-INPUT validation, replies to \`owner_id\`/\`owner_gen\`, typed status for unknown/malformed ops, never exits on bad input.
- \`userspace/proc-mgr-parity.c\` (new) — deterministic parity probe (\`S PROCMGR seq=...\` lines), no fork from the probe itself.
- \`src/kernel/log/dmesg_catalog.hpp\` — one new TASK row \`+21\` (RPC bound expired) + its single push site in the proxy.
- \`src/kernel/nexios_config.h\` — \`CONFIG_PROC_MGR\` default becomes debug-1/release-0 (idle-monitor precedent, \`nexios_config.h:1393-1399\`), **last step, gated**.
- \`docs/specs/microkernel-transition.md\` — §7.1 role count 13→14; §8.8 State/Protocol/Verification; §15 F11 row \`new\` → \`in-progress\` (not closed); Appendix A deferred note.
- NOT touched, deliberately: \`src/kernel/syscall/syscall_handlers_process.cpp\` / \`_misc.cpp\` (Stage A wrappers stay thin — the legs live in \`ProcSvc\`, exactly the split Stage A created), \`src/kernel/syscall/syscall.hpp\` (no new number, GETPID stays a FAST member), \`src/kernel/task/boot_sdf.hpp/.cpp\` (row already in tree), \`src/kernel/task/tcb_control.cpp\` (asserts already 14, gate untouched), \`src/kernel/task/scheduler_terminate.cpp\` (funnel hook already correct), all \`src/kernel/test/**\` (test sanctity — QE lane).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-PROC-1 (caller context)** — unchanged and load-bearing: clone, ELF load/swap, waitpid block/reap, terminate and trap-frame restore all run in the calling task with its \`regs\`. No trap frame, TCB pointer or kernel stack pointer crosses the PD boundary; the wire struct carries ids/generations only (\`proc_wire.hpp:53-66\`).
- **INV-PROC-2 (bounded Call)** — every RPC goes through \`IPC::send_sync_bounded_err(..., CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS)\` (\`ipc.cpp:1163\`), one call per syscall, never per waitpid loop iteration.
- **INV-PROC-3 (no lock across RPC)** — handlers take no lock before the verdict call: \`syscall\` context enters with no CNode/derivation/hold-table lock held; the hold-table leaf is taken and released inside \`hold()\`, which runs **after** the verdict. Never hold \`scheduler_lock_\` across an RPC (CODING_STYLE §11.1).
- **INV-PROC-4 (bounded hold table)** — 32 fixed slots, zero allocation; overflow = \`PROC_ERR_CAP_TABLE_FULL\` with the child destroyed pre-publication.
- **INV-PROC-5 (generation-matched release)** — unchanged contract; **now non-vacuous** because step 1 gives cloned TCBs a real generation. Funnel takes derivation leaf + holder-CNode leaf only, under \`IrqGuard\`, idempotent, no logging (CODING_STYLE §12.1/§12.2).
- **INV-PROC-6 (one Q9 gate, PD never calls it)** — unchanged: \`tcb_control_authorize\` is byte-for-byte untouched; stop enforcement stays in the invoking client's context on the existing \`parent_id\` branch. Holds remain CREATOR-scope lifecycle tracking with zero stop power.
- **INV-PROC-7 (OFF-build behavior identity)** — with \`CONFIG_PROC_MGR=0\` every new block compiles out (proxy TU empty, \`ProcSvc\` bodies are the Stage A text, taskdefs row absent, config default unchanged until step 10). Behavior-identity, not binary identity.
- **INV-PROC-8 (ResourceTracker)** — TcbCap folds into \`cap_objects\`; no new resource class; \`proc_tcb_caps::snapshot_reset()\` must be wired by QE before the flip so every path ends at delta 0.
- **INV-PROC-9 (SDF accounting)** — the proc-mgr row declares 0 device caps, 0 Untyped, 0 death watches; \`sdf_check_bounds\` accepts it; \`mk_inv_static_bounds\`-class demand checks stay green.
- **INV-PROC-10 (arch parity)** — no new syscall number, no new \`CONFIG_ARCH_*\` branch, 48 B wire assert unchanged; all three arches build.
- **INV-PROC-11 (NEW — verdicts are deny-only)** — a proc-mgr verdict may **refuse** an operation and may never **grant** one: after an OK verdict the kernel still runs every existing gate (Liu-Leyland admission on fork, path validation on exec, \`parent_id\` scan on waitpid, target resolution + signal-class rules on kill, per-TCB handler rules on signal). Consequence: a compromised or lying PD can only reduce availability, never widen authority (this is what keeps INV-PROC-6 true once a PD actually answers).
- **INV-PROC-12 (NEW — pre-publication hold)** — on the fork path the hold is installed **before** \`Scheduler::add_task_err\` publishes the child. Hold failure ⇒ \`TaskControlBlock::destroy(child)\` with no \`publish_task_death\`, no death record, no waiter wake (mirrors the existing admission-denial destroy at \`proc_svc.cpp:65-69\`). Admission failure after a successful hold ⇒ release the hold, then destroy. No path leaves a hold naming an unpublished task.
- **INV-PROC-13 (NEW — generation before publication)** — every TCB gets its generation from the single allocator before it becomes reachable (\`init_task_common\` today; \`clone\` from step 1). No code treats \`generation == 0\` as "unset" (grep evidence: only \`wdog_gen\`, posix-time slot gens and debug-binding gens use 0 sentinels).
- **Concurrency boundaries**: (a) hold-table leaf \`g_hold_lock\` is taken alone, never across CNode/derivation calls (INV-DERIV-1); (b) the death funnel runs under \`IrqGuard\` with \`scheduler_lock_\` released and must dereference **no** foreign task's \`CNode*\` (step 2); (c) the reclaim drain runs in syscall context where the holder is the caller and therefore provably alive; (d) \`g_live_holds\` is atomic from every context (CODING_STYLE §11.6); (e) the PD's own message loop blocks in \`ipc_recv\` — no spin, no priority mutation, no allocation.

## STEP-BY-STEP CHANGES

### Foundation (fixes that must land before any caller exists)

1. \`src/kernel/task/task.cpp\` — extract \`static uint32_t next_task_generation() noexcept { return __atomic_fetch_add(&s_next_generation, 1U, __ATOMIC_RELAXED); }\` next to \`s_next_generation\` (:444); use it in \`init_task_common\` (:447, same value stream) and add \`tcb->generation = next_task_generation();\` in \`clone()\` immediately after \`TCB_WRITE(tcb, id, Scheduler::alloc_id());\` (:1813). Comment cites drift 4 (hold key + \`tcb_control.cpp:104\` + \`resolve_live_target\` need a non-degenerate generation; snapshot restore rewinds pids). \`spawn_thread\` (:2239) is **out of scope** — record it as an S3 follow-up note only.
2. \`src/kernel/proc/proc_tcb_caps.cpp/.hpp\` — close both code-r1 S3s:
   - \`HoldSlot\` gains \`bool reserved\` and \`bool reclaim_pending\`.
   - \`hold()\`: the claim loop (:111-119) picks the first slot with \`!occupied && !reserved\` and writes \`holder_id/holder_gen/target_id/target_gen\` + \`reserved = true\` **under the table lock**. Install path unchanged. The publish block (:199-209) re-checks under the table lock that the slot is still \`reserved\` and still names this holder/target; if not (funnel cleared it — the child died mid-hold) ⇒ \`remove_if(cnode_slot, slot_gen)\` + \`held_cap->release()\` + \`PROC_ERR_STALE_PID\`; else \`occupied = true; reserved = false\`. Every earlier failure path clears \`reserved\` under the lock before returning.
   - \`release_on_death()\`: capture **no** \`CNode*\` and no job array of pointers. Entries naming the dead holder (or dead target) are cleared in the index under the table lock; target-death entries additionally set \`reclaim_pending\` (the slot/gen pair stays in the entry, bounded by \`kMaxHeldTcbCaps\`, allocation-free). This removes the unlock-then-\`remove_if\` UAF window without a \`ScopedRef\` pin (a pin would violate the \`release_all_objects()\` refcount==1 contract, \`kernel_object.hpp:151-156\`).
   - New file-local \`drain_reclaim(TaskControlBlock &holder)\` called at the top of \`hold()\`, \`lookup()\` **read-only** (lookup must not mutate — see note) and \`release()\`: harvest \`reclaim_pending\` entries whose holder matches under the table lock, clear them, unlock, then \`holder.get_cspace()->remove_if(slot, gen)\` (gen-checked, idempotent, \`remove_if\` contract honoured). Called only from \`hold()\` and \`release()\` — those are the paths where the holder is the calling task and provably alive.
   - \`snapshot_reset()\` also clears \`reserved\`/\`reclaim_pending\`.
   - Header: document that a hold's CNode slot may be reclaimed lazily (capacity is bounded and fail-closed, never a panic), and that a dead holder's slots are reclaimed by \`CNode::dispose\`.

### Kernel verdict path

3. \`src/kernel/proc/proc_mgr_proxy.hpp/.cpp\` (all inside \`#if CONFIG_PROC_MGR\`):
   - \`TaskControlBlock *proc_mgr_holder() noexcept\` — \`find_task(get_proc_mgr_pid())\` + \`magic == TCB_MAGIC\` + not \`TERMINATED/REAPED\`; \`nullptr\` when down.
   - \`errors::ProcError proc_mgr_verdict(ProcMgrOp op, uint64_t arg0, uint64_t arg1, uint64_t arg2) noexcept\` — builds \`ProcMgrRpc\` with \`op\`, \`epoch = proc_mgr_epoch()\`, \`owner_id = cur->id\`, \`owner_gen = cur->generation\`, \`arg0..arg2\`, zero reserved fields; calls \`proc_mgr_call\`; on OK runs \`ProcSvc::handle_decision(reply)\` (\`proc_svc.cpp:579\`) and returns its code (so a forged/mismatched echo is refused as \`BAD_REQUEST\`, and \`STOP/RESUME\` stay \`STOP_REFUSED\`); \`NO_SERVER\`/\`RPC_BOUND\`/\`BAD_REQUEST\` pass through. No lock held anywhere in the function (INV-PROC-3).
   - In the \`PROC_ERR_RPC_BOUND\` branch push the new dmesg row (\`kDmesgBase_TASK + 21\`, WARN, "Proc-mgr RPC bound expired") — outside any lock.
   - Probe seams for the QE class (procfs-srv shape, \`procfs_srv.cpp:121-166\`): \`int64_t proc_mgr_probe_bind();\` and \`int64_t proc_mgr_probe_loopback(uint64_t payload, uint64_t *echo_out);\` — no production caller, whole TU already knob-guarded.

4. \`src/kernel/proc/proc_svc.cpp\` — the per-op legs, all in caller context, all \`#if CONFIG_PROC_MGR\` so the OFF build is the Stage A text unchanged:
   - File-local helpers: \`static errors::ProcError ask_verdict(ProcMgrOp op, uint64_t a0, uint64_t a1, uint64_t a2)\` = \`proc_mgr_proxy::proc_mgr_verdict(...)\`; \`static bool denied(errors::ProcError e)\` → \`e != PROC_ERR_OK\`.
   - \`fork()\` (:54): after the \`!regs\` gate and **before** \`clone\`: \`ask_verdict(FORK, 0, 0, 0)\`; non-OK ⇒ \`SYS_ERR_GENERIC_FAIL\` (no child ever created).
   - \`exec()\` (:403): after the existing early path/argv presence gates and before the first filesystem touch: \`ask_verdict(EXEC, arg0, arg1, arg2)\`; non-OK ⇒ fail. Validation, read, \`elf::exec_into_current\`, the id-preserving swap and the file-page frees are untouched.
   - \`waitpid()\` (:130): exactly once, after \`cur\` is resolved (:138-140) and **before** the \`for(;;)\` loop (:148): \`ask_verdict(WAITPID, target_pid, 0, 0)\`; non-OK ⇒ fail. Loop body (\`find_wait_child\`, \`collect_zombie_child\`, the #590 second-residence collect, the #593 current-on-any-cpu guard, \`thread_join_try_claim\`, WNOHANG) stays **statement-identical** — no per-iteration RPC.
   - \`kill()\` (:473): after the sig-range, SIG_NONE, \`validate_id\` and \`find_task\` gates (:477-488) and before the self/other disposition branch (:489): \`ask_verdict(KILL, target_pid, sig, 0)\`; non-OK ⇒ fail. The fatal/handler classification (\`signal_is_fatal\` + \`has_signal_handler\`), \`pending_signals\`, and both #593 legs (\`terminate_err_from_trap\` + \`switch_away_from_terminating\`) stay byte-identical (INV-PROC-11: the verdict cannot reclassify).
   - \`signal()\` (:528): after the sig-range / fatal / SIG_NONE gates and before the per-TCB install: \`ask_verdict(SIGNAL, sig, arg1, 0)\`.
   - \`sigreturn()\` (:547): **no** RPC. Add the documented policy/mechanism split (the trap-frame restore is mechanism; the handler table it restores is proc-mgr-owned via \`signal\`).
   - \`getpid()\` (:573): unchanged \`syscall_task()->id\` read; comment states the writer discipline — the fork path is the only producer of a new identity (\`alloc_id\` in \`clone\`) and \`exec\` preserves it, so there is no PD-side replica to diverge; GETPID remains a FAST member (\`syscall.hpp:229\`) with no lock, no IPC, no allocation.

5. \`src/kernel/proc/proc_svc.cpp\` \`fork()\` hold wiring (INV-PROC-12):
   - After a successful \`clone\`: \`auto *holder = proc_mgr_proxy::proc_mgr_holder();\` — \`nullptr\` ⇒ \`TaskControlBlock::destroy(child)\` + fail (typed \`NO_SERVER\` is already the verdict's answer; the holder re-resolve can still fail if the PD died between the verdict and the clone).
   - \`int slot = proc_tcb_caps::hold(*holder, child->id, child->generation, &hold_err);\` — \`slot < 0\` ⇒ \`TaskControlBlock::destroy(child)\` (never published, no death record, no 33rd-child visibility) + fail. \`CAP_TABLE_FULL\` already logs \`+18\` inside \`hold()\`.
   - \`Scheduler::add_task_err(*child)\` unchanged; on denial ⇒ \`proc_tcb_caps::release(*holder, child->id, child->generation)\` then \`destroy\` (extends the existing :65-69 branch).
   - \`return child->id;\`

### Boot row (must land before the MOVE takes effect — §8.10 literal ordering)

6. \`src/kernel/task/taskdefs.cpp\`:
   - \`#include <kernel/proc/proc_mgr_proxy.hpp>\` under \`#if CONFIG_PROC_MGR\` (top include block).
   - New hook block after the procfs-srv block (:289):
     \`\`\`
     #if CONFIG_PROC_MGR
     void pd_restart_proc_mgr(uint64_t pid) {
         auto *t = kernel::Scheduler::find_task(pid);
         if (t == nullptr || t->magic != TaskControlBlock::TCB_MAGIC) return;
         if (!kernel::sdf::sdf_rebind(*t, kernel::sdf::PdRole::PROC_MGR)) return;
         kernel::proc::proc_mgr_proxy::proc_mgr_on_restart(pid);
     }
     uint64_t pd_pid_proc_mgr() { return kernel::proc::proc_mgr_proxy::get_proc_mgr_pid(); }
     void pd_set_proc_mgr(uint64_t pid) { kernel::proc::proc_mgr_proxy::set_proc_mgr_pid(pid); }
     #endif
     \`\`\`
     (the hook order — \`sdf_rebind\` first, then the cell/epoch bump — mirrors \`pd_restart_procfs_srv\`; \`proc_mgr_on_restart\` stores the pid **and** bumps the epoch, so a client's stale-epoch reply is refused by \`proc_mgr_call\`'s identity check.)
   - Row, appended immediately after the \`procfs-srv\` row (:437), before the disabled \`shell\` row:
     \`\`\`
     #if CONFIG_PROC_MGR
     // proc-mgr: process policy PD (issue #564 F11, strangler step b).
     // Sporadic server SS(1,10,0); policy + verdicts only, no device caps.
     // Restart class: restartable (Q12 critical = supervisor + mem-mgr).
     {"proc-mgr", TaskType::SPORADIC_SERVER, true, nullptr,
      "bin/proc-mgr.c.elf", 4, 10, 0, 1, 10, 0, "proc-mgr",
      pd_set_proc_mgr, pd_pid_proc_mgr, 1, 0, false,
      ServerMode::SPORADIC, pd_restart_proc_mgr, 13},
     #endif
     \`\`\`
   - **Placement rationale (append, not insert):** PIDs are allocated in table order by \`reboot_from_table\`, so inserting earlier would renumber every PD (vfsd 2, iocd 3, …) and break procfs/parity goldens and the \`/etc/rc\` expectations. §8.10's ordering is satisfied by **step** order (row before the MOVE legs), not by table position.
   - \`sdf_role\` bound (\`:498-504\`) already accepts PROC_MGR — no edit. \`CONFIG_MAX_DAEMONS\` (16): enabled SS at the debug flip = vfsd+iocd+watchdogd+ramdiskd+proc-mgr = 5, in bound. (The all-knobs-ON configuration exceeds 16 with or without proc-mgr — pre-existing, out of scope, note only.)

### Userspace PD + probe

7. \`userspace/proc-mgr.c\` (new, procfs-srv.c shape, no allocation):
   - \`#include <stddef.h> <stdint.h> <stdio.h> <unistd.h> <string.h> <syscall.h> <ipc.h>\`; \`MSG_DAEMON_READY 0xF0000001\`, \`MSG_TYPE_REPLY 0xF0000002\`.
   - \`struct ProcMgrRpc\` mirroring \`proc_wire.hpp\` (op, flags, rsvd, epoch, owner_id, owner_gen, arg0, arg1, arg2, status_op, status, rsvd3) + the \`char check[sizeof(...) <= 48 ? 1 : -1]\` static shape assert (procfs-srv.c:33-36).
   - \`main()\`: shape assert → \`(void)getpid()\` → \`ipc_send(1, MSG_DAEMON_READY, NULL, 0, 0)\` → server loop (blocking \`ipc_recv(rbuf, sizeof rbuf)\`, the in-tree PD precedent \`procfs-srv.c:44\`; the only permitted unbounded form here because the wait is scheduler-mediated, CODING_STYLE §11.3).
   - R-INPUT next: \`rc < 0\` ⇒ continue; \`(unsigned long)rc != req.op\` ⇒ continue (torn/stale payload — kernel msg type must equal body op); after echo fields, \`req.rsvd != 0 || req.rsvd3 != 0\` ⇒ \`status = -22\`.
   - Dispatch: BIND → 0; LOOPBACK → \`rep.arg0 = req.arg0\`, 0; FORK/EXEC/WAITPID/KILL/GETPID/SIGNAL/SIGRETURN → 0 with \`rep.arg0/arg1\` echoing the request (so the kernel can prove request/reply pairing); STOP/RESUME → \`status = -1\` (STOP_REFUSED equivalent); default/unknown → \`status = -22\` (typed, no exit).
   - Reply: \`rep.op\`, \`epoch\`, \`owner_id\`, \`owner_gen\`, \`status_op = req.op\` echoed; \`ipc_send(req.owner_id, MSG_TYPE_REPLY, &rep, sizeof rep, 0)\` — never PID 1 (#441).
   - Header comment: Stage B policy is allow-with-typed-refusals; the kernel's gates remain authoritative (INV-PROC-11), so this file is a policy seam, not an authority.

8. \`userspace/proc-mgr-parity.c\` (new): deterministic single-shot probe printing vfs-srv-parity-shaped lines (\`printf("S PROCMGR seq=%d op=%s rc=%d\n", ...)\`): seq0 \`getpid\`, seq1 \`kill(getpid(), 0)\` (null signal), seq2 \`waitpid(-1, &st, WNOHANG)\`, seq3 \`signal(SIGKILL, ...)\` (must refuse), seq4 done. **No fork/exec from the probe** (it would leave a task behind and perturb the suite); the class drives fork/exec parity from the kernel side.

### Observability + knob

9. \`src/kernel/log/dmesg_catalog.hpp\`: add \`{ErrorSubsystem::TASK, kDmesgBase_TASK + 21, LogSeverity::WARN, "Proc-mgr RPC bound expired"}\`. Live TASK offsets verified by grep: +1, +7, +9, +10, +17, +18, +19, +20 (\`+18/+19/+20\` are Stage A's). Push site: the proxy's RPC_BOUND branch (step 3). Re-grep before commit (LEARNINGS #354).
10. \`src/kernel/nexios_config.h\` — **last, gated**: replace the \`CONFIG_PROC_MGR\` default block (\`:1686-1693\`) with the idle-monitor pattern (\`:1393-1399\`): \`#if defined(CONFIG_DEBUG) → 1, #else 0\`. Preconditions, in order: (a) QE parity class green on `proc_elf`/`proc_mgr`; (b) §8.8 interim waiver in the tree (it is); (c) three-arch builds green with the knob ON; (d) release legs re-verified with the knob OFF. The MOVE rows in T-1 only "take effect" once the parity class is green — this diff's default flip is the last gate, not the first.

### Docs

11. `docs/specs/microkernel-transition.md`:
    - §7.1 (`:429`): role list "…PROCFS_SRV (13)" → "…PROCFS_SRV, PROC_MGR (14)".
    - §8.8.1 State: PD exists (`userspace/proc-mgr.c`, `CONFIG_PROC_MGR`, debug 1 / release 0 until the 2x3 matrix); 20-24 + 31-32 dispatch through `ProcSvc` with the verdict leg when the knob is on, mechanism in caller context.
    - §8.8.3 Protocol: add the deny-only verdict rule (INV-PROC-11), the fork pre-publication hold (INV-PROC-12), the (pid,gen) release from the M1 funnel, and the lazy self-drain reclaim.
    - §8.8.5 Verification: `proc_mgr_parity`, `pid_reuse_safe`, `proc_mgr_crash_restart`, `proc_mgr_stop_authority`, `proc_mgr_hold_full`, `proc_mgr_death_visible` (reusing `mk_death_visible_bounded`).
    - §8.8.2 waiver: keep the interim text; retitle it as an in-tree fact with the F11a pointer.
    - §15 (`:919`) F11 row: `new` → `in-progress` (never `closed` here), note "MOVE rows flip on parity green; release default stays 0".
    - Appendix A: Stage B entry — kernel-composed `clone` fork retained, carve composition deferred to F11a (Q1 option (a)).

## TEST STRATEGY

Test sanctity: this dev lane lands **no** test files. Hand-off sketch for the QE lane (stub-first: `JARVIS_TEST_PASS()` + doc-block + pseudocode first, then assertions):

- New test class `proc_mgr` (registry row + `test_expected_counts.hpp` row first; compile-time sum checked).
  - `proc_mgr_parity` (`proc_elf` or `proc_mgr`): per op, identical return values and observable state on the kernel path vs the `CONFIG_PROC_MGR` path — fork ⇒ child id, `parent_id`, `page_table_ != parent`; kill ⇒ `pending_signals` bit; waitpid ⇒ exit code + reaped state; signal ⇒ handler installed; getpid ⇒ equal ids; sigreturn ⇒ identical trap-frame fields. Uses the new probe seams (`proc_mgr_probe_bind`/`_loopback`) for BIND/LOOPBACK plus `userspace/proc-mgr-parity.c` output for the user-side view.
  - `pid_reuse_safe`: kill + reap a child, force a pid recycle (snapshot-restore rewind), assert `proc_tcb_caps::lookup` fails typed on the stale (pid,gen), and that `resolve` refuses on generation mismatch; tracker delta 0. **This is the test that step 1 makes satisfiable.**
  - `proc_mgr_crash_restart` (`servers`): kill the PD; assert `restart_stale_daemons` → `pd_restart_proc_mgr` → `sdf_rebind` + epoch bump, that in-flight clients get a typed error (never a hang, #560 path), that holds are re-minted for live children only, tracker delta 0.
  - `proc_mgr_stop_authority` (`cap_tcb`): stop authority stays in caller context — a CLIENT caller stops its own child through the unchanged `parent_id` branch; typed, side-effect-free refusals for self, kernel task, idle, mem-mgr, and a cross-subtree client caller. The PD-as-caller path is deliberately NOT tested (it cannot pass the unchanged gate by construction). Complements `mk_supervisor_stop`.
  - `proc_mgr_hold_full` (`cap_tcb`): the 33rd concurrent hold fails `CAP_TABLE_FULL`, the 33rd child is destroyed pre-publication (no zombie, no death record, no waiter wake), no panic, tracker delta 0.
  - `proc_mgr_death_visible` (`proc_elf`): a PD-originated fault kill is reported through the M1 death record within the Q4 bound — must **call** `mk_death_visible_bounded` (`test/test_mk_death.cpp:249`), never duplicate it.
- Test-isolation seam (testbed lane, currently missing — code-r1 V9): `test_isolate.cpp` restore must call `kernel::proc::proc_tcb_caps::snapshot_reset()` adjacent to `cap::derivation_snapshot_reset()` (`:1285`) and **before** `MemPool::restore_pool_data` (LEARNINGS #320 ordering: release pins before the byte rewind).
- Existing classes to run, reason in line: `proc_elf` (135/81 — direct blast radius: process lifecycle, ELF load, pml4 clone, pt_merge, libc wrappers), `core_task` (TCB clone/teardown/funnel, death record), `capability` (cap_tcb + cap_vspace + cap_broker + sdf_bounds groups; catches the role-count bump and the SDF demand check), `cap_tcb` / `cap_vspace` (the F4/F5 surfaces the hold reuses), `ipc` (bounded Call semantics, INV-PROC-2), `servers` (compose/rebind/restart of the new row), `selftest` (CI gate).
- Gates before any audit request: `tools/meson/build.sh --arch {x86_64,aarch64,riscv64}` all green (INV-PROC-10, and the knob-ON build too); `make check-style` Errors 0 (571+ files); the classes above on x86_64 debug with the knob ON and OFF; off-x86 legs of `proc_elf`/`servers`; one append-only `test-history.txt` row per class run (measured times only). H2-family zero-S-line stalls get one retry and a `STATUS` row, never a PASS.
- Flake discipline: the documented H2-family TIMEOUT/PANIC flake is unchanged by this work; a zero-S-line stall is a harness/TCG event, recorded as such.

## SIL 3 / SAFETY RISKS

- [S1] A verdict leg on fork/exec/waitpid/kill could change ordering or resume a foreign context — mitigation: INV-PROC-1 (mechanism in caller context, verdict only), INV-PROC-11 (deny-only: every kernel gate still runs after an OK verdict), invariant `proc_mgr_parity` per op, and a `git diff -U0` review proving the #590/#593 legs are statement-identical.
- [S1] Generation-degenerate hold keys (drift 4) would let a stale hold resolve against a recycled pid in tests and leave `tcb_control`'s gen check vacuous in production — mitigation: step 1 (single allocator, assigned before publication), grep evidence that no consumer treats `generation == 0` as unset, and `pid_reuse_safe` as the acceptance test.
- [S1] Hold-table claim/publish race (code-r1 S3 #1) becomes reachable once `hold()` has a caller: two concurrent holds can publish into the same slot and silently drop one CNode slot + skew `g_live_holds` — mitigation: step 2 reserved-slot protocol with a re-check at publish, rollback on every failure path, idempotent no-op when the funnel already cleared the slot.
- [S1] Death-funnel dereference of a foreign holder's `CNode*` after unlock (code-r1 S3 #2) = UAF on SMP holder teardown — mitigation: step 2 removes all foreign-pointer capture (index-only in the funnel, gen-checked lazy reclaim in the holder's own syscall context); no `ScopedRef` pin (it would violate the `release_all_objects()` refcount==1 contract, trading a UAF for a teardown assert).
- [S1] Hold leak on any fork/teardown/restart path (double hold, missed release) — mitigation: INV-PROC-12 (pre-publication hold, release-then-destroy on admission failure), 32-slot fixed table, funnel release by (pid,gen), `snapshot_reset()` seam, tracker delta 0 on every path.
- [S1] Stop authority widened by a second predicate, a PD-held SUPERVISOR cap, or a PD-authored disposition — mitigation: INV-PROC-6 (gate byte-identical, PD never a caller), INV-PROC-11 (verdict cannot reclassify fatal vs handler; policy may only refuse), CREATOR_ONLY caps (`tcb_cap.cpp:82`), `proc_mgr_stop_authority` refusals via client callers.
- [S1] PD death while holding caps, or a boot order where clients fork before the PD is live (every fork then fails closed) — mitigation: row is `enabled` in `reboot_from_table` (spawned before `init` runs `/etc/rc`), restart via `pd_restart_proc_mgr` + epoch bump, typed `NO_SERVER`/`PEER_DIED` to waiters (never a hang), `proc_mgr_crash_restart` test, and the flip gated behind the parity class.
- [S2] The debug default flip (step 10) lands before the seam wiring is green — mitigation: strict ordering (parity green → waiver in tree → 3-arch knob-ON builds → flip), release stays 0, and the flip is a one-line revert.
- [S2] Extra IPC round-trip per process syscall changes timing-sensitive tests — mitigation: one RPC per call (never per waitpid iteration), no RPC on getpid, `CONFIG_PROC_MGR_RPC_TIMEOUT_TICKS` bound with a typed refusal, and the OFF build identical by construction.
- [S2] Wire-struct drift between `proc_wire.hpp` and `userspace/proc-mgr.c` — mitigation: `static_assert(sizeof == 48)` on both sides (the userspace one via the `char check[...]` idiom), per-op field audit, torn-payload `rc != req.op` check.
- [S2] Schedulability: the row's SS budget enters the enabled set (vfsd 2/10 + iocd 3/10 + watchdogd 1/10 + ramdiskd 2/10 + proc-mgr 1/10 = 0.9 > the n=5 Liu-Leyland bound; the bound is informational today, briefing §12 gotcha 11) — mitigation: documented in the row comment, raised as Q-B1; not a blocker because admission is not enforced, but it must not be silently ignored.
- [S3] dmesg offset collision — mitigation: +21 verified free by grep; re-grep before commit (LEARNINGS #354).
- [S3] `spawn_thread` still leaves `generation == 0` — mitigation: out of scope here, recorded as a follow-up note in the step-1 comment (threads are not proc-mgr holds; the Q9 gen check for threads stays as weak as it is today).
- [S3] `proc-mgr-parity` probe shipped in release — mitigation: probe is a probe (no auto-start row); if noise appears, guard it with the parity knob as plan-564 rev 3 S3 notes.

## OPEN QUESTIONS

- Q-B1 (does not block; ships the 1/10 variant): plan-564 rev 3 step 15 says the row is "priority 4, period 10, 1/1 sporadic". A literal SS(1,1,0) is a 100 % budget — every existing policy-only PD (term-srv, log-srv, cap-broker, wdog-mon, procfs-srv) uses SS(1,10,0), and `taskdefs_server_params_valid` only bounds `budget <= period`. OPTIONS: (a) SS(1,10,0) — 10 % budget, consistent with the other five policy PDs, admits a bounded WCET estimate; (b) literal SS(1,1,0) — 100 % budget, which makes proc-mgr a "always runnable" server and distorts the Liu-Leyland sum to 1.8. RECOMMENDATION: (a) SS(1,10,0) with priority 4 / period 10 as stated; confirm at review. NOT VIABLE: leaving the field implicit — the row must carry an explicit budget/period per §7.1.
- Q-B2 (does not block; ships deny-only): plan-564 rev 3 step 16 says the proxy "supplies the disposition class (fatal vs handler)" for kill. A PD-authoritative class would let a compromised policy PD suppress a fatal signal on any target (an authority widening that INV-PROC-6's deny-only spirit forbids). This plan implements **deny-only** — the PD may refuse a kill, the kernel keeps classifying. OPTIONS: (a) deny-only (as planned here); (b) PD-authoritative class with a new invariant + audit. RECOMMENDATION: (a); record (b) as the policy stage's opening decision if the owner wants signal-class policy later.
