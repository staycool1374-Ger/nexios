# PLAN: #557 / F4 — TCB_CONFIGURE / TCB_SUSPEND / TCB_RESUME (N1–N3): cap-gated task control — REV 2

## REVISION HISTORY

| Rev | Date | Outcome | Trigger |
|---|---|---|---|
| 1 | 2026-11-19 | REJECTED (3 blockers, 4 S3) | audits/design-review-557-r1.md |
| 2 | 2026-11-19 | this document | addresses every r1 finding + 1 S1 found while re-verifying |

Per-finding resolution (r1 -> rev 2)

| r1 finding | Resolution in rev 2 |
|---|---|
| [S1] resume leaves the per-task watchdog permanently disarmed | CLOSED (new): suspend preserves wdog_expiry_tick + wdog_last_kick_tick and only clears wdog_armed; the saved expiry is latched in admin_wdog_expiry_tick and resume re-arms shifted by the suspension (expiry += elapsed, last_kick += elapsed, ++wdog_gen). New INV-ADM-11. WATCHDOG_CREATE on a parked target is refused typed. Paused != revoked is physically observable (expires= stays non-zero while disarmed; a WdogCap dispose/revoke zeroes it — cap/wdog.cpp:110-123). TEST 8 rewritten to pin the ARMED end-state. Owner notification: new catalogued dmesg record SCHED+21. |
| [S2] authority not Q9-faithful (self-grantable watch slots) | CLOSED (new): DeathNotify::is_watching is REMOVED from the authority predicate. Authority = creator-parent OR a compose-minted TcbCap carrying scope SUPERVISOR bound to (caller.id, caller.generation); that scope is settable only by the kernel mint path (no syscall producer, unchanged F4 fact). Exact rule stated in INV-ADM-12 / step 6. TEST 14 rewritten; new negative leg asserts a self-installed watch confers nothing. |
| [S2] "never mem-mgr" shipped unenforceable | CLOSED (new): predicate becomes three enforceable layers — kernel-task refusal, a registered mem-mgr pid cell (0 today, truthful), and Untyped-possession (the spec's defining property of mem-mgr, §6.1) via a bounded cap::cnode_holds_type walk over the target CSpace. Residual framed as an owner decision with two concrete options, not a deferral (OPEN QUESTIONS Q-r2-b). |
| [S3-1] #80 SIL 3 feeder must be evidence, not a plan | CLOSED: promoted to a gated deliverable (step 18) — the #557 comment must exist and its URL be cited in the spec-sync commit message in the same change. |
| [S3-2] guard precision / composition statement | CLOSED: exact insertion point fixed (scheduler_registry.cpp:514, before the liveness capture at :518 and the READY write at :520, hence before mailbox_publish :536, the remote direct enqueue :567-569, the wake-while-current deferral :549-563 and the enqueue_ready fallthrough :572). Composition with the debugger park is now ENFORCED, not merely documented: suspend refuses a debug-parked target (BUSY) and debug_park_stop refuses an admin-parked target (step 12). |
| [S3-3] #588 carve-out undocumented | CLOSED: INV-ADM-13 + scheduler.md subsection with anchors (scheduler_lifecycle.cpp:420, :440, :494-500, :502) and the join sibling (task.cpp:156-157). |
| [S3-4] benign freezes unstated | CLOSED: INV-ADM-14; both asserted in TEST 8's doc block and in TEST 3. |
| NEW (found this session, S1) | Scheduler::scan_deadlines iterates ALL tasks and skips only TERMINATED (scheduler_watchdog.cpp:93-135, skip at :109) — removing the deadline node does NOT protect a parked periodic task: it would be reported (or killed, per CONFIG) as missing every deadline while stopped. Fixed by an explicit admin_parked skip + TEST 8 leg. |

Everything not listed above (stop-guarantee structure, 6-field latch shape, closed-set numbering
99/100/101 + MAX 102, scope cuts, resolve shape, syscall/ABI/libc steps, doc set, cross-arch by
construction) is UNCHANGED from rev 1 and not relitigated.

## BASE / EVIDENCE

    Base: main @ cc1a67046049fce9b6aa06948c1d5a20b781ef53 ("#591: off-x86 selftest unblocked"),
          read via the GitHub commits API this session. Rev 1 declared 4fc5ed779; main has moved
          since, so EVERY anchor touched by this plan was re-opened by direct read at this tree and
          the line numbers below are the ones observed now.
    Anchors re-verified this session (file:line as read):
      cap/wdog.cpp:110-123 disarm_target (armed=false, expiry=0, ++gen, claim released)
      cap/wdog.hpp:36-67   WdogCap (target_pid/target_gen; single-owner claim per pid)
      syscall_handlers_watchdog.cpp:55-77  resolve_watchdog_target (cap-gen checked, NO state check)
        :85-103 arm_watchdog (period, last_kick, expiry, armed, ++gen; saturating now+period at 89-91)
        :127-149 sys_watchdog_kick (refuses INVALID_STATE when !armed at :140-141)
      watchdogd.cpp:47-75  grant_supervision — the ONLY WdogCap producer in the tree (kernel daemon)
      scheduler_watchdog.cpp:70-74 watchdog_due; :355-388 scan_watchdogs_locked (one-shot disarm, no
        state filter); :395-424 fire_stall_watchdog; :93-135 scan_deadlines (all-task walk, :109
        skips TERMINATED only)
      idle_monitor.cpp:230-268 idle_escalate_stall -> fire_stall_watchdog (armed/expiry driven)
      task.hpp:830-865 wdog fields; :364-434 ctor init list; :1072-1107 debug latch fields;
        :1403 static_assert(sizeof(TaskControlBlock) <= kMemPoolMaxBlockSize)
        (mempool.hpp:34-39: 8192, aarch64 16384)
      death_notify.cpp:55-93 watch() — :60-62 lets actor==sup self-install on ANY live watched
      death_notify.hpp:118-121 is_watching
      boot_sdf.hpp:41-54 CapKind (7 kinds, no stop/supervise); :75-89 PdRole (13 values, no
        supervisor/mem-mgr); :119-120 kPdRoleCount
      driver_pds.cpp:24-57 role->pid cells, kMaxTrackedPdRoles=6 (roles >= 6 report 0), no reverse
        lookup
      cap_types.hpp:37-57 CapType (16 values, kCapTypeLast=SchedContext)
      cap.hpp:59-165 CNode (slots[], lock_, install/remove/peek/slot_gen); cap.cpp:173-186 peek;
        cap.cpp:202-216 lookup takes cspace->lock_ (leaf); cap.cpp:509/569/635 foreign-source takes
      scheduler_registry.cpp:512-573 set_task_ready (514 IrqGuard, 518 liveness, 520 READY write,
        522 idle_note_progress, 536 mailbox_publish, 549-563 wake-while-current, 567-569 remote direct
        rq_for(...).enqueue, 572 enqueue_ready)
      scheduler_queue.cpp:290-352 enqueue_ready (deferral 336-350); :357-388 enqueue_ready_inner
      scheduler_tick.cpp:450-480 / :488-512 / :520-538 wake loops (READY at 463/502/532); :541
        PosixTime::reapply_wakes; :1100-1121 debugger park block; :1377-1413
        resched_park_until_dispatched (loop 1394-1404)
      scheduler_smp.cpp:108-148 mailbox_drain (READY at :139, enqueue at :146)
        CORRECTION to rev 1: that site was labelled "balancer"; it is the SCHED-IPI mailbox drain and
        the only READY write in scheduler_smp.cpp (verified by grep). The guard set is unchanged.
      posix_time.cpp:532-569 reapply_wakes (READY :554)
      scheduler_debug.cpp:61-83 debug_park_stop; :88-106 debugger_resume
      scheduler_terminate.cpp:46-80 publish_task_death; :82-105 terminate_err entry; :135/:250 wake
        sites
      scheduler_lifecycle.cpp:397-442 deliver_wait_status (store :420, slot clear :440); :445-511
        wake_waiting_parent (frame write 494-500, waiting_child_pid=0 :502, dequeue + set_task_ready
        483-484)
      task.cpp:139-165 thread_join_note_termination (join_status_/join_ready_ :156-157);
        :1745/:2162 parent_id set at creation (kernel-only); :2579-2616 cleanup (REAPED :2595, wdog
        disarm 2598-2605); :2618 complete_teardown
      syscall.hpp:127-129 (REPLY_RECV=97, CAP_DELETE=98, MAX_SYSCALL=99)
      syscall_handlers_thread.cpp:259-295 thread_join_resolve (the resolve shape reused verbatim)
      procfs.cpp:463-491 pid_watchdog_read (armed=/period=/last_kick=/expires=)
      dmesg_catalog.hpp:280-291 kDmesgCatalog_SCHED (+16..+20 taken, +21/+22 free)
      test_expected_counts.hpp:300-306 (core_sched 127, core_task 52, capability 161); :379-381

    Retrieval: rev-1 artifacts cited, not re-run for areas already covered (graphify queries 1-4 and
      the vault searches in the rev-1 header remain valid evidence). Re-run ONLY for the newly
      touched areas:
      5. graphify query_graph "watchdog disarm and re-arm supervision: WdogCap, wdog_armed, idle
         monitor stall escalation, fire_stall_watchdog, watchdog daemon" -> 175 nodes; surfaced
         WdogCap, disarm_target(), fire_stall_watchdog, idle_monitor.cpp, the Driver-PD supervision
         contract doc.
         DISPOSITION: USED — confirmed the disarm path owns the armed state and that no in-tree
         re-arm-on-resume precedent exists, so supervision must be re-derived on the resume edge.
      6. graphify query_graph "who may suspend or stop another task: supervisor authority, DeathNotify
         watch slots, capability gates, parent_id creator relation" -> 57 nodes; surfaced
         DeathNotify / watch / is_watching / Slot, docs/specs/death-notify.md "Supervisor Protocol",
         zombie-list.md "7.8 child->parent_id = 0 in wake_waiting_parent".
         DISPOSITION: USED — proves the watch slots are the ONLY "supervision" record in-tree and that
         they are runtime-mutable (hence not compose-checked); also surfaced the parent_id-orphaning
         semantics that bound the creator branch.
      7. obsidian search ("watchdog re-arm supervision pause supervisor authority mem-mgr exclusion",
         "task suspend supervisor capability authority matrix") -> 0 matches each (vault "jarvis").
         DISPOSITION: gap recorded, not a skip — the normative text is
         docs/specs/microkernel-transition.md (§6.1 mem-mgr ownership :497-512, §9.3 critical servers
         :716, §16 Q9 :898 — all read this session) plus docs/specs/driver-pds.md and
         docs/specs/idle_monitor.md.

## AFFECTED FILES

- src/kernel/task/task.hpp — admin latch fields: the rev-1 six plus admin_wdog_expiry_tick (rev 2,
  S1 fix); ctor init list (:364-434); ADMIN doc block; AdminPrevState enum beside TaskState.
- src/kernel/task/scheduler.hpp — declarations: admin_suspend_err / admin_resume_err / admin_absorb_wake
  / admin_parked_of + counters + the two new must-read-0 counters. TaskFields untouched (INV-ADM-6).
- src/kernel/task/scheduler_suspend.cpp (NEW) — park, resume (deadline AND wdog shift + re-arm), latch,
  deferred-stop consumer helper, admin_absorb_wake.
- src/kernel/task/scheduler_registry.cpp — set_task_ready: absorb guard at :514, i.e. before the
  liveness capture (:518), the READY write (:520), idle_note_progress (:522), the remote
  mailbox_publish (:536), the wake-while-current deferral (:549-563), the remote direct
  rq_for(...).enqueue (:567-569) and the local enqueue_ready fallthrough (:572).
- src/kernel/task/scheduler_queue.cpp — enqueue_ready (:290) / enqueue_ready_inner (:357) owning refusal.
- src/kernel/task/scheduler_tick.cpp — absorb guards in the three level-triggered loops (:463, :502,
  :532) + the admin deferred-stop block after the #225 block (:1100-1121).
- src/kernel/task/scheduler_smp.cpp — mailbox_drain absorb guard (:139). Label corrected (rev 2).
- src/kernel/task/scheduler_debug.cpp — (rev 2) debug_park_stop refuses an admin-parked target (:67-70
  guard area) so the two park owners are mutually exclusive by construction.
- src/kernel/task/scheduler_watchdog.cpp — (rev 2) scan_deadlines skips an admin-parked task (:110) so a
  stopped periodic task is not reported/killed; doc block states the pause + the shift contract.
- src/kernel/task/scheduler_terminate.cpp — terminate funnel clears the admin latch (:46-80, :135, :250).
- src/kernel/task/task.cpp — cleanup() clears the admin latch beside the existing wdog disarm
  (:2598-2605), before complete_teardown (:2618).
- src/kernel/time/posix_time.cpp — reapply_wakes absorb guard (:553, before the READY write :554).
- src/kernel/task/tcb_control.hpp (NEW) — handle -> live TCB resolve, the exact Q9 authority rule, N1
  field semantics, the mem-mgr predicate; pure functions so syscall and tests share one gate.
- src/kernel/task/tcb_control.cpp (NEW) — implementation (cap lookup, authority, role/mem-mgr predicate,
  mem-mgr pid cell, typed mapping).
- src/kernel/task/tcb_control_errors.hpp (NEW) — X-macro errors::TcbCtlError (cap_errors.hpp:32-59 shape).
- src/kernel/cap/tcb_cap.hpp (NEW) / src/kernel/cap/tcb_cap.cpp (NEW) — TcbCap (target_id, target_gen,
  + scope / grantee_id / grantee_gen), ThreadCap shape (thread_cap.hpp:39-72, .cpp:39-68),
  ResourceTracker track_cap_object_add/remove.
- src/kernel/cap/cap.hpp / cap.cpp — (rev 2) bool cnode_holds_type(CNode*, CapType) — bounded
  CONFIG_CSLOT_COUNT walk under cs->lock_ (same leaf take as lookup, cap.cpp:202-216); no allocation, no
  reference taken, no scheduler lock held at the call site.
- src/kernel/syscall/syscall_handlers_watchdog.cpp — (rev 2) arm_watchdog refuses a parked target typed
  (SYS_ERR_SCHED_INVALID_STATE) inside its existing IrqGuard block (:92-101).
- src/kernel/cap/carved_object.hpp — doc-only: carved TCB has no live binding at F4; the bind seam is
  M12/M14.
- src/kernel/log/dmesg_catalog.hpp — (rev 2) two catalog rows SCHED+21 (supervision paused, WARN) and
  SCHED+22 (supervision resumed, WARN) appended at :291; severities mirrored in lookup_severity's SCHED
  arm.
- src/kernel/syscall/syscall.hpp — TCB_CONFIGURE=99, TCB_SUSPEND=100, TCB_RESUME=101, MAX_SYSCALL=102
  (:128-129); three handler declarations; three syscall_table_ rows after CAP_DELETE.
- src/kernel/syscall/syscall_handlers_tcb.cpp (NEW) — sys_tcb_configure / sys_tcb_suspend / sys_tcb_resume.
- src/kernel/syscall/syscall_errors.hpp — 1000-block TCBCTL_* X-rows (next free range after 907).
- src/libc/syscall.h — SYS_TCB_* defines + prototypes beside SYS_REPLY_RECV (:307, :349), MAX_SYSCALL
  drift comment (:354, 99 -> 102); then regenerate src/libc/gen/nexios_syscalls.{c,h} with
  tools/gen_syscall_stubs.py (generated files are touched only by the generator).
- docs/specs/microkernel-transition.md — §3.1 T-1 rows 99/100/101 (:190 append point), §3.3 N1-N3
  resolved (:196-198), §3.4 counts (certified 54 -> 57, MAX_SYSCALL 102), §2 row (:103), §8.7 item 1
  (:608), §13.3 (mk_supervisor_stop), §15 F4, Appendix A row — all in the SAME commit as step 15.
- docs/specs/scheduler.md — admin-stop section: state table, INV-ADM-1..16, deadline/wdog pause+shift
  rule, user-boundary rule, Q9 authority matrix, debugger/admin mutual exclusion, #588 carve-out,
  benign-freeze list.
- docs/specs/cspace.md — TcbCap row incl. scope/grantee binding and its F2 derivation/cascade relation.
- docs/specs/death-notify.md — one line: watch slots confer death notification only, never stop
  authority (cross-reference INV-ADM-12).
- docs/irqguard-ledger.md — 2 new Sites rows (admin_suspend/admin_resume) + the cnode-walk site.
- NOT touched: src/kernel/nexios_config.h (no new key — the mem-mgr identity is a runtime pid cell, not a
  config knob), src/kernel/test/**, src/meson.build (new .cpp auto-discovered at :122-134).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-ADM-1 (single writer to runnable).** While admin_parked is set, NO path may set the target READY
  or link it into a runq; the only legal exit is Scheduler::admin_resume_err. Two layers: (a) semantic —
  every wake-decision site calls admin_absorb_wake() BEFORE writing READY; (b) structural —
  enqueue_ready/enqueue_ready_inner refuse an owning parked TCB, restore BLOCKED, latch
  (adm_wake_backstop_count_, must-read-0 outside the dedicated negative test).
- **INV-ADM-2 (absorb is a latch, never a drop).** An absorbed wake sets admin_wake_pending exactly once,
  consumed exactly once by resume (-> READY). Reads/writes of admin fields happen only with IF=0
  (IrqGuard) or scheduler_lock_ held; all writers hold scheduler_lock_ (§6.2 item 7 order unchanged).
- **INV-ADM-3 (no READY/BLOCKED desync).** A parked task is never in_ready_queue_, never in_edf_queue_,
  never ready_deferred_ (§11.2).
- **INV-ADM-4 (the BLOCKED channel is preserved, never torn down).** Suspend does NOT touch
  mutex/semaphore/event/queue/pager/IRQ waiter arrays, waiting_*, reply_wait*, blocked_in_recv,
  blocked_on_pager_fault, waiting_child_*.
- **INV-ADM-5 (death wins).** TERMINATED/REAPED are terminal for the latch: the terminate funnel and
  cleanup() clear all admin fields; resume refuses a dead target (§12.1).
- **INV-ADM-6 (snapshot neutrality).** The seven fields are transient (never in TaskFields), zero at every
  memset/ctor site, byte-rewound by MemPool::restore_pool_data. No new ResourceTracker type.
- **INV-ADM-7 (no new O(n) work on RT paths).** The absorb guard is O(1) inside loops that already exist;
  new scans are O(CONFIG_MAX_CPUS)=8 (latch refusal) and DeadlineList::insert/remove O(<=64) in syscall
  context; the mem-mgr walk is O(CONFIG_CSLOT_COUNT)=O(64) in AUTHORITY (syscall) context only, never in
  an ISR or a tick path.
- **INV-ADM-8 (lock discipline).** admin_suspend_err/admin_resume_err are task-context only:
  ENSURE(scheduler_lock_depth_own() == 0) + IrqGuard + SchedulerLockGuard (debugger_resume precedent,
  scheduler_debug.cpp:90-92). They never reschedule(), block, allocate or log-flood. N1 calls
  set_priority/set_affinity_err OUTSIDE the lock (each takes it itself, non-recursive).
- **INV-ADM-9 (#582 interaction).** A task halted in resched_park_until_dispatched
  (scheduler_tick.cpp:1394-1404) is already stopped: the loop breaks on state != BLOCKED, so suspend
  leaves it halted and resume's READY releases it. A latch-armed BLOCKED runner (INV-PARK-2) is refused
  with TCBCTL_ERR_BUSY, never silenced.
- **INV-ADM-10 (arch neutrality).** All new logic is shared C++: no per-arch asm, no new apply site, no new
  trap-frame handling (INV-MK-5/INV-MK-10 by construction).
- **INV-ADM-11 (supervision is paused and restored, never revoked) — rev 2, closes r1 S1.**
  Suspend of a watchdog-armed target screens supervision but does not destroy it:
  admin_wdog_expiry_tick := wdog_expiry_tick (latched), then wdog_armed := false; ++wdog_gen
  (wdog_expiry_tick / wdog_last_kick_tick are LEFT INTACT — unlike disarm_target, cap/wdog.cpp:116-118,
  which zeroes the expiry). Resume, when admin_wdog_expiry_tick != 0 (the was-armed predicate: an armed
  wdog always has a non-zero expiry, a disarmed one is 0): wdog_expiry_tick := admin_wdog_expiry_tick +
  elapsed (saturating at UINT64_MAX, the same fail-safe clamp arm_watchdog uses at :89-91),
  wdog_last_kick_tick += elapsed (saturating — keeps the (last_kick, expiry) interval semantics intact
  for procfs and idle_monitor), wdog_armed := true, ++wdog_gen. Hence: (i) the #41 hang guarantee is
  restored with the phase preserved — a target suspended with N ticks of budget left resumes with N
  ticks, never a fresh full period and never a permanent revocation; (ii) wdog_gen is bumped on both
  edges so any stale idle-seam expect_gen (idle_monitor.cpp:243-264) skips; (iii) paused and revoked stay
  distinguishable to the owner without any new syscall: paused = armed=0, expires=<non-zero>, task
  BLOCKED + admin park; revoked/disposed = armed=0, expires=0 (cap/wdog.cpp:117); (iv) an already-overdue
  expiry (admin_wdog_expiry_tick <= admin_suspend_tick, reachable only through test-fabricated arms)
  resumes already-due and fires on the next scan — fail-safe, recorded in scheduler.md.
- **INV-ADM-12 (Q9 authority — exact rule) — rev 2, closes r1 S2-2.** tcb_control_authorize evaluates, in
  order, and every refusal is typed with ZERO side effects:
  1. caller.id == target.id -> SELF.
  2. !target.is_user_ -> KERNEL_TASK (covers idle, monitor, kernel daemons incl. watchdogd, harness).
  3. Scheduler::is_idle_task(target) -> IDLE.
  4. tcb_control_role_protected(target) -> PROTECTED_ROLE (INV-ADM-15).
  5. CREATOR / Q9 client-stop branch: target.parent_id == caller.id -> ALLOW. parent_id is kernel-assigned
     at creation only (task.cpp:1745, :2162) and is cleared on orphan/reap (scheduler_lifecycle.cpp:509,
     :629) — it is NOT caller-writable, so this branch cannot be forged and it fails closed once the
     relation ends.
  6. SUPERVISOR / Q9 server-stop branch (the explicit override path): ALLOW only if the resolved TcbCap
     for the target carries scope == Authority::SUPERVISOR AND grantee_id == caller.id AND
     grantee_gen == caller.generation. scope is set exclusively by the kernel-only factory
     TcbCap::create_supervised(target_id, target_gen, grantee_id, grantee_gen); TcbCap::create() (the
     F4-minted, creator-facing form) always yields scope == CREATOR_ONLY. Copy/grant preserve scope and
     grantee, so a delegate cannot exercise it — Q9's "multi-supervisor delegation is a post-v0.6
     protocol".
  7. otherwise -> NO_AUTHORITY (Q9 "cross-subtree attempts fail closed with a typed error").
  Explicitly NOT authority: ipc::DeathNotify::is_watching is removed from the predicate (its slots are
  self-grantable, death_notify.cpp:60-62), and raw WdogCap possession is not stop authority either
  (watchdog authority only). Consequence, stated for the owner: with no syscall producing TcbCaps
  (unchanged F4 fact) and no compose path minting SUPERVISOR-scope caps until M5/M12, branch 6 is
  deny-all in production and provably so (fail-closed BAD_HANDLE / NO_AUTHORITY), while being fully
  exercisable by tests through the internal factory — the same status the reviewer already accepted for
  the TcbCap producer.
- **INV-ADM-13 (stop-guarantee boundary: scheduling state, not handoff data) — rev 2, closes r1 S3-3.**
  A park freezes the target's SCHEDULING state. It does NOT freeze data a waker legitimately hands it:
  wake_waiting_parent still stores the child's status into the parked parent's user slot
  (scheduler_lifecycle.cpp:420, fault-armed #588 path), clears waiting_child_status (:440), patches the
  saved return-value slot (:494-500) and clears waiting_child_pid (:502) while the READY transition is
  absorbed; thread_join_note_termination likewise writes join_status_/join_ready_ (task.cpp:156-157).
  Correct on resume: the parent re-enters its wait loop, the rescan observes the zombie and returns
  without blocking again. Documented in scheduler.md beside the absorb rule.
- **INV-ADM-14 (benign freezes) — rev 2, closes r1 S3-4.** While parked: (a) the sporadic-server budget is
  untouched — nothing consumes it (BLOCKED) and replenishment is bounded by its period, so no budget is
  created or lost; (b) remaining_ticks is frozen, which is consistent with deadline_ticks being frozen
  and only SHIFTED by the elapsed suspension at resume (both frozen, phase preserved) — the "suspension
  buys budget" direction is closed by construction.
- **INV-ADM-15 (never mem-mgr — enforceable) — rev 2, closes r1 S2-3.** tcb_control_role_protected(t) is
  true iff ANY of: (a) a registered mem-mgr pid exists and t.id == it — the cell
  tcb_control_mem_mgr_pid() returns 0 today, which is the truthful state (no user-space mem-mgr exists;
  memory is kernel-internal until M4/F10 #563) and 0 matches nothing; (b) t.is_user_ and the task's
  CSpace holds a live CapType::Untyped cap — the spec's defining property of the mem-mgr ("mem-mgr PD
  owns all free memory as Untyped", §6.1:497-512), checked by
  cap::cnode_holds_type(t.get_cspace(), CapType::Untyped) under the CNode leaf lock; (c) the
  compile-time tripwire static_assert(kPdRoleCount == 13) (boot_sdf.hpp:119-120) breaks the build the
  moment a mem-mgr/supervisor role row is added, forcing this predicate to be revisited in the same
  commit. Layer (b) is the durable part: when M4 hands the root Untyped to a user PD, that PD becomes
  unsuspendable AUTOMATICALLY, with no table to remember to populate.
- **INV-ADM-16 (deadline supervision is paused, not violated) — rev 2, new S1.** While admin_parked,
  scan_deadlines (scheduler_watchdog.cpp:93-135) skips the task; resume's shift makes deadline_ticks
  future-consistent. Without this, removing the deadline node is insufficient because the scan walks
  all_tasks_, not deadline_list_.

## STEP-BY-STEP CHANGES

1. **task.hpp — latch fields.** After has_utrap_frame / debug_rearm_va (:1077-1107) add, each with a
   transient-state doc block: bool admin_parked; bool admin_wake_pending; bool admin_stop_requested;
   uint8_t admin_prev_state; uint64_t admin_suspend_tick; uint64_t admin_deadline_ticks;
   uint64_t admin_wdog_expiry_tick (the rev-2 addition for INV-ADM-11). Add all seven to the ctor init
   list (:364-434, zero/false) and to every TCB memset site (create/create_user/clone/elf finalize —
   LEARNINGS #3). Add enum class AdminPrevState : uint8_t { READY=0, RUNNING, BLOCKED } beside TaskState
   with value pins; add NO new TaskState enumerator. Verify
   static_assert(sizeof(TaskControlBlock) <= kMemPoolMaxBlockSize) (:1403) still holds; if the ~28 added
   bytes break the pool class, collapse admin_deadline_ticks/admin_wdog_expiry_tick into
   uint64_t admin_saved_ticks[2] with named accessors — do NOT shrink the latch set.
2. **cap/tcb_cap.hpp + .cpp — authority object.** class TcbCap : public KernelObject,
   kTcbCapTag = 0x50414354U, target_id, target_gen, uint8_t scope (enum class Authority : uint8_t
   { CREATOR_ONLY = 0, SUPERVISOR = 1 }), uint64_t grantee_id, uint32_t grantee_gen. static TcbCap
   *create(id, gen) -> scope CREATOR_ONLY, grantee 0; static TcbCap *create_supervised(id, gen,
   grantee_id, grantee_gen) -> scope SUPERVISOR, called only from kernel compose code (no syscall path,
   exactly as watchdogd::grant_supervision is the sole WdogCap producer). Both factories validate: live
   task, magic, generation match. dispose() (track_cap_object_remove + MemPool::free), revoke(),
   is_shared() == true. ThreadCap shape (thread_cap.cpp:39-68) so F2 derivation/revoke cascade applies
   with no new machinery.
3. **cap/cap.hpp + cap.cpp — bounded type query (rev 2).** bool cnode_holds_type(CNode *cs, CapType
   want): null-cs -> false; SpinLockGuard<sync::SpinLock> guard(cs->lock_); then walk
   i < CONFIG_CSLOT_COUNT calling cs->peek(i, want); true on first hit. Doc-block the leaf-lock rule
   (same take class as cap::lookup, cap.cpp:202-216; must not be called with scheduler_lock_ held — the
   authority gate runs before the mechanism lock).
4. **cap/carved_object.hpp — doc seam.** Carved TCB carries no live target binding at F4; the bind seam
   (retype-created TCB -> TcbCap) lands with retype-only creation (M12/M14). No code change.
5. **task/tcb_control_errors.hpp (NEW).** X-macro errors::TcbCtlError (cap_errors.hpp:32-59 shape): OK,
   INVALID_ARGS, BAD_HANDLE, STALE_TARGET, DEAD, SELF, KERNEL_TASK, PROTECTED_ROLE, NO_AUTHORITY,
   NOT_SUSPENDED, ALREADY_SUSPENDED, BUSY, UNSUPPORTED, IDLE.
6. **task/tcb_control.hpp/.cpp (NEW) — the gate.**
   tcb_control_resolve(caller, handle, err): ensure_cspace(); cap::lookup(cspace, handle, CapType::TCB,
   CapRights::WRITE); TcbCap tag check; CAPTURE scope/grantee_id/grantee_gen/target_id/target_gen BEFORE
   release(); Scheduler::validate_id; find_task; TaskControlBlock::is_valid; generation equality;
   TERMINATED/REAPED filter — the exact shape of thread_join_resolve
   (syscall_handlers_thread.cpp:259-295), returning the resolved authority tuple.
   tcb_control_authorize(caller, target, const TcbControlAuthority &cap, err): implement INV-ADM-12
   steps 1-7 verbatim, in order (the POD parameter avoids five swappable scalars — see S3-b).
   tcb_control_role_protected(target): implement INV-ADM-15 (mem-mgr pid cell + cnode_holds_type walk +
   the static_assert tripwire next to #include <kernel/task/boot_sdf.hpp>).
   tcb_control_mem_mgr_pid() / tcb_control_set_mem_mgr_pid(uint64_t): a file-static cell (0 today), the
   hook M4/F10 will set; __atomic accessors, no lock, no config key.
   Pure/read-only apart from the leaf CNode take; no scheduler lock; unit-testable directly.
7. **task/scheduler.hpp + scheduler_suspend.cpp — mechanism.** Declare/implement
   admin_suspend_err(TaskControlBlock&), admin_resume_err(TaskControlBlock&),
   admin_absorb_wake(TaskControlBlock&), admin_parked_of(const TaskControlBlock&),
   adm_wake_absorbed_count(), adm_wake_backstop_count(), adm_stop_deferred_count(), adm_resumed_count(),
   adm_wdog_paused_count(), adm_wdog_rearmed_count(), adm_deadline_paused_count().
   SUSPEND (ENSURE(depth==0) + IrqGuard + SchedulerLockGuard): reject invalid magic / not in id_table;
   target == current_task() -> SELF; TERMINATED/REAPED -> DEAD; already parked -> ALREADY_SUSPENDED;
   debug_parked -> BUSY (rev 2: two park owners are mutually exclusive); latch-armed on any CPU
   (gs::lockpark_latched_task, 8-CPU bounded) -> BUSY; state == RUNNING -> latch admin_stop_requested
   (RELEASE) + adm_stop_deferred_count_ (OK only if queue_target(target) == sched_cpu(), else BUSY);
   state == WAITING -> BUSY (never assigned in-tree, fail closed); READY -> dequeue_ready, prev=READY,
   state=BLOCKED; BLOCKED -> prev=BLOCKED (no queue work); then admin_prev_state,
   admin_suspend_tick = arch::Timer::ticks(), admin_deadline_ticks = target.deadline_ticks,
   admin_wdog_expiry_tick = target.wdog_expiry_tick (INV-ADM-11: latch, do NOT zero), admin_parked = true
   (RELEASE), admin_wake_pending = false, wdog_armed = false; ++wdog_gen (expiry/last_kick preserved),
   deadline_list_.remove(target), and if admin_wdog_expiry_tick != 0 push dmesg SCHED+21 and
   ++adm_wdog_paused_count_.
   RESUME (same envelope): TERMINATED/REAPED -> DEAD; !admin_parked -> NOT_SUSPENDED (typed, no side
   effect); elapsed = now - admin_suspend_tick (saturating); deadline_ticks = admin_deadline_ticks +
   elapsed and deadline_list_.insert when period_ticks > 0 and deadline_ticks > 0; WDOG RE-ARM per
   INV-ADM-11 (admin_wdog_expiry_tick != 0 -> shift expiry and last_kick, wdog_armed = true, ++wdog_gen,
   ++adm_wdog_rearmed_count_, dmesg SCHED+22); clear admin_parked (RELEASE) BEFORE any enqueue; if
   admin_wake_pending or admin_prev_state != BLOCKED -> state = READY + enqueue_ready(target); clear
   admin_wake_pending / admin_stop_requested / admin_wdog_expiry_tick; ++adm_resumed_count_.
8. **Wake-site absorb guards (INV-ADM-1 layer a).** Insert "if (admin_absorb_wake(task)) {
   ++counter; continue/return; }" immediately BEFORE the READY write at: scheduler_registry.cpp:514
   (set_task_ready — after the IrqGuard, before the liveness capture :518, hence before :520/:522/:536/
   :549-563/:567-569/:572); scheduler_queue.cpp:340 (wake-while-current); scheduler_tick.cpp:462, :501,
   :531; posix_time.cpp:553; scheduler_smp.cpp:139 (mailbox_drain — NOT a balancer). admin_absorb_wake is
   O(1), takes no lock, DEBUG-asserts IF==0 or scheduler_lock_held(), and sets admin_wake_pending
   without touching state.
9. **Structural backstop (INV-ADM-1 layer b).** In enqueue_ready (:290) and enqueue_ready_inner (:357)
   after validate_task_ptr: if admin_parked -> admin_wake_pending = true, force state = BLOCKED, unlink
   defensively, ++adm_wake_backstop_count_, DEBUG one-shot warn, return. Document as the ONE intentional
   deviation from INV-RQ-2's byte-identical refusal (the TCB is owned here), existing so a missed site
   cannot strand a READY-queued suspended task.
10. **Terminate/teardown clearing (INV-ADM-5).** scheduler_terminate.cpp at the terminate funnel (before
    publish_task_death) and task.cpp cleanup() beside the existing wdog disarm (:2598-2605): clear
    admin_parked / admin_wake_pending / admin_stop_requested / admin_wdog_expiry_tick and drop the
    deadline node when it was parked. Death records, causes, generation and the zombie path stay
    byte-identical.
11. **Deferred-stop consumer (task-context stop of a RUNNING target).** In rate_monotonic_schedule
    immediately after the #225 debugger block (:1100-1121), a second block with the same preconditions:
    cpu < CONFIG_MAX_CPUS, admin_stop_requested (ACQUIRE), state == RUNNING,
    tick_pc < CONFIG_USER_SPACE_LIMIT (user boundary only) -> clear the request, prev=RUNNING,
    state=BLOCKED, dequeue_ready, admin_parked = true, admin_suspend_tick, latch admin_wdog_expiry_tick,
    disarm wdog, deadline_list_.remove. Debugger park keeps precedence.
12. **Debugger/admin mutual exclusion (rev 2).** In scheduler_debug.cpp debug_park_stop (:61-83), inside
    the existing lock/guard region after the :67-70 checks: if admin_parked -> return false (the attach is
    refused; the caller's existing "attach failed" path handles it). Reciprocally, the suspend body
    refuses debug_parked with BUSY (step 7). No other debug-path change.
13. **Deadline-scan pause (rev 2, INV-ADM-16).** In scheduler_watchdog.cpp scan_deadlines add
    "if (task->admin_parked) continue;" immediately after the TERMINATED skip (:109-110), with a doc
    block stating the pause contract and that the resume shift restores phase; bump
    adm_deadline_paused_count_ (informational, not must-read-0). No change to watchdog_due or
    scan_watchdogs_locked (the disarm already covers those).
14. **Watchdog arm refusal (rev 2, INV-ADM-11).** In syscall_handlers_watchdog.cpp arm_watchdog, inside
    the existing IrqGuard block (:92-101) and before the field writes: if
    __atomic_load_n(&t->admin_parked, __ATOMIC_ACQUIRE) return
    static_cast<uint64_t>(-errors::SYS_ERR_SCHED_INVALID_STATE) — a stopped PD must never be declared
    hung. sys_watchdog_kick needs no change (a suspended target is disarmed, so :140-141 already returns
    INVALID_STATE); state that in the header doc block.
15. **syscall.hpp — numbers and dispatch.** Append TCB_CONFIGURE = 99, TCB_SUSPEND = 100, TCB_RESUME = 101;
    MAX_SYSCALL = 102 (:129); declare the three handlers; add three syscall_table_ rows after CAP_DELETE.
    4-arg form only: CONFIGURE(handle, field_mask, value_a, value_b), SUSPEND(handle), RESUME(handle).
    FAST-tier bits untouched (test_syscall_fastpath.cpp:84/138 green by construction).
16. **syscall_handlers_tcb.cpp (NEW).** sys_tcb_configure: syscall_task() null -> INVALID_ARGS; resolve ->
    authorize (step 6, using the captured cap scope/grantee); mask == 0 or bits >= 4 -> INVALID_ARGS (no
    silent ignore); bit0 -> value_a <= CONFIG_PRIORITY_CEILING else INVALID_ARGS, then
    Scheduler::set_priority; bit1 -> Scheduler::set_affinity_err (map SCHED_ERR_* via the existing 800
    block); bit2 (SchedContext bind) / bit3 (fault endpoint) -> TCBCTL_ERR_UNSUPPORTED.
    sys_tcb_suspend / sys_tcb_resume: resolve -> authorize -> admin_*_err -> map. Every return is a typed
    code; no raw -1.
17. **syscall_errors.hpp — typed face.** 1000-block TCBCTL_* = 1000 + TcbCtlError plus inline
    SyscallError from_tcb_control_error(uint64_t) beside from_task_error (:153). NEW numbers get a NEW
    contract (F2's frozen face applied to an existing number — state the distinction in the handler doc).
18. **libc + ABI drift + #80 feeder (r1 S3-1).** src/libc/syscall.h: define SYS_TCB_CONFIGURE 99 /
    SYS_TCB_SUSPEND 100 / SYS_TCB_RESUME 101, three long tcb_* prototypes beside SYS_REPLY_RECV
    (:307-:349), MAX_SYSCALL comment (:354, 99 -> 102); then python3 tools/gen_syscall_stubs.py (never
    hand-edit generated files). IN THE SAME CHANGE: (a) commit the microkernel-transition.md §3.1/§3.3/
    §3.4 edits from AFFECTED FILES; (b) post the T-1 SIL 3 justification as a #557 comment (three
    numbers, cap-gated, no ambient authority, deny-never-degrade, Q9 subtree rule) and cite that comment
    URL in the spec-sync commit message. The comment is a GATE item: it must exist before the audit
    request, exactly as #585 did.
19. **Docs.** scheduler.md: INV-ADM-1..16 (state table, deadline/wdog pause+shift rule, user-boundary rule,
    the Q9 authority matrix incl. the "watch slots confer nothing" line and the debugger/admin mutual
    exclusion, the #588/join carve-out (INV-ADM-13) with anchors, the benign-freeze list (INV-ADM-14));
    cspace.md: TcbCap row with scope/grantee binding; death-notify.md: watch-slots-confer-no-stop-authority
    line; irqguard-ledger.md: 2 Sites rows + the cnode walk.
20. **Build/verify.** tools/meson/build.sh on all three arches (build + link), make check-style Errors 0,
    then the classes below. No nexios_config.h key.

## TEST STRATEGY

- New tests (QE, testbed; stub-first JARVIS_TEST_PASS() then real asserts; three wirings per LEARNINGS
  #309/#170: file + run_group decl + class entry, plus the test_expected_counts.hpp row). Placement:
  mechanism legs in core_task (present in the x86_64, aarch64 AND riscv64 measured lists,
  tools/meson/list_tests.py:24-43); authority/cap legs in capability (M11 gate class, §14:849).

  run_tcb_control_group() (core_task):
  1. tcb_suspend_ready_parks_and_dequeues — READY+queued target -> admin_parked true, state BLOCKED,
     !in_ready_queue_, !in_edf_queue_; after a tick the target's run counter is unchanged.
  2. tcb_resume_prev_ready_reenqueues — resume -> READY, queued, runs to completion; counter +1.
  3. tcb_suspend_blocked_absorbs_wake_then_resumes — target blocked on a Semaphore; suspend; post ->
     adm_wake_absorbed_count +1, state still BLOCKED, admin_parked still true, task did NOT run; resume ->
     READY, wait returns, run counter +1. Same shape for a parent blocked in waitpid whose child exits
     (the #588 path — asserts the INV-ADM-13 carve-out delivers status while parked and that the rescan
     returns on resume). Also asserts INV-ADM-14(a): the sporadic-server budget is byte-identical across
     the park.
  4. tcb_suspend_absorb_backstop_refuses_link — call Scheduler::enqueue_ready(parked) directly ->
     refused, state restored BLOCKED, adm_wake_backstop_count == 1, in_ready_queue_ false,
     orphan_rescue_count unchanged.
  5. tcb_resume_not_suspended_typed — live non-parked target -> NOT_SUSPENDED, state byte-identical.
  6. tcb_suspend_dead_target_typed — TERMINATED and REAPED targets both -> DEAD.
  7. tcb_suspend_deferred_running_boundary — latch admin_stop_requested on a RUNNING user task; a tick
     with a user-mode PC parks it; a tick with a kernel PC does not.
  8. tcb_resume_rearms_watchdog_and_shifts_deadline — REWRITTEN (r1 S1). Doc block asserts INV-ADM-11 and
     INV-ADM-14 explicitly: sporadic budget untouched (no consumption while BLOCKED, bounded
     replenishment) and remaining_ticks frozen, consistent with the deadline shift (both frozen, phase
     preserved). Periodic user target with an armed watchdog (period P, expiry E, wdog_gen = g) and a
     deadline; suspend at tick S -> assert wdog_armed == false, wdog_expiry_tick == E (PRESERVED, not
     zeroed), wdog_gen == g + 1, admin_wdog_expiry_tick == E, deadline node removed; advance ticks past E
     and past deadline_ticks -> NO watchdog kill, NO deadline_miss, no dmesg TIMING+6/+7 record (proves
     INV-ADM-16); resume at tick R -> assert wdog_armed == TRUE, wdog_expiry_tick == E + (R - S),
     wdog_last_kick_tick == saved_last_kick + (R - S), wdog_gen == g + 2, deadline_ticks ==
     saved_deadline + (R - S), remaining_ticks unchanged, dmesg SCHED+21 then SCHED+22 present, and
     SYS_WATCHDOG_KICK on the resumed target returns 0 (supervision is live, not revoked). Second half: an
     UNARMED target must resume with wdog_armed == false and wdog_gen unchanged after resume (no phantom
     arm). NEVER asserts a disarmed end-state.
  9. tcb_resume_shift_saturates — fabricate admin_wdog_expiry_tick / admin_deadline_ticks near UINT64_MAX
     -> resume clamps to UINT64_MAX (no wrap to an immediate fire).
  10. tcb_watchdog_arm_refused_while_parked — suspend an armed-candidate target, then
      SYS_WATCHDOG_CREATE(period, pid, handle) -> SYS_ERR_SCHED_INVALID_STATE and the target is NOT killed
      by a subsequent tick scan; SYS_WATCHDOG_KICK -> INVALID_STATE (already-disarmed path).
  11. tcb_suspend_parked_runner_released_by_resume — a task halted in resched_park_until_dispatched is
      suspended (stays halted) and released by resume without tripping a park/apply counter.
  12. tcb_suspend_latch_armed_refused — fabricate a latch via gs::lockpark_latch_arm -> BUSY; reset with
      gs::lockpark_reset_for_test().
  13. tcb_debugger_admin_park_mutual_exclusion (rev 2) — suspend a debug-parked target -> BUSY, debug park
      intact; debug_park_stop on an admin-parked target -> false, admin_parked intact.

  run_cap_tcb_group() (capability):
  14. mk_supervisor_stop — REWRITTEN (r1 S2-2). Matrix: creator (target.parent_id == caller.id) allow;
      creator's authority disappearing on orphan -> NO_AUTHORITY; foreign without a supervisor cap ->
      NO_AUTHORITY; foreign that installed a DeathNotify::watch on the target -> STILL NO_AUTHORITY (the
      r1 blocker's regression anchor); foreign holding a create_supervised TcbCap bound to
      (caller.id, caller.gen) -> allow; the same cap held by a DIFFERENT task (granted onward) ->
      NO_AUTHORITY (grantee binding); self SELF; kernel target KERNEL_TASK; idle IDLE; revoked TcbCap
      (CAP_REVOKE 53) BAD_HANDLE; deleted TcbCap (CAP_DELETE 98) BAD_HANDLE; cap for a recycled generation
      STALE_TARGET; READ-only cap BAD_HANDLE.
  15. tcb_never_mem_mgr_predicate (rev 2) — a user helper whose CSpace holds a minted CapType::Untyped ->
      PROTECTED_ROLE for SUSPEND and CONFIGURE; after the Untyped cap is deleted the same target is
      suspendable; tcb_control_set_mem_mgr_pid(id) -> that pid is PROTECTED_ROLE regardless of caps
      (reset to 0 at teardown).
  16. tcb_cap_derivation_cascade — CAP_GRANT/COPY a TcbCap then CAP_REVOKE the parent -> the derived cap's
      suspend fails closed (INV-MK-7: the named cap survives); and a SUPERVISOR-scope cap copied onward
      cannot be used by the copy's new holder.
  17. tcb_config_priority_and_affinity — priority re-buckets through set_priority; affinity applies and
      clamps as set_affinity_err does; a parked target stays configurable.
  18. tcb_config_unsupported_and_bad_mask — mask 0 and bits >= 4 -> INVALID_ARGS; bit2/bit3 ->
      UNSUPPORTED; priority > CONFIG_PRIORITY_CEILING -> INVALID_ARGS.
  19. sys_tcb_surface_and_numbering — Syscall::handle(99/100/101, ...) dispatch reachable;
      MAX_SYSCALL == 102; 102 rejected with SYS_ERR_INVALID_NUMBER; ABI_VERSION unchanged.
  - Negative-argument sweep (QE): each of 99/100/101 with arg0 in {0, UINT64_MAX, 0xDEADBEEF...} returns a
    typed error and never faults (test_syscall_fuzz.cpp:56-96 covers only YIELD/SEND/PRINT).
- Existing classes to run, fix-one-at-a-time, x86_64 debug first: core_task (52 -> +13), capability
  (161 -> +6), proc_elf (131), core_sched (127 — park/apply counters, wake invariants), ipc (the 40
  monitor-adjacent wake paths funnel through set_task_ready), core_sync (mutex/semaphore/event/queue wake
  legs), selftest.
- Cross-arch gate (mandatory, same cycle): tools/meson/build.sh --arch {aarch64,riscv64} build+link green,
  then --class core_task, --class core_sched, --class ipc, --class capability on BOTH off-x86 arches
  (capability is absent from the aarch64/riscv64 measured lists — run it explicitly and report it, as
  prior cycles did; a flake gets an identical-binary rerun). Full evidence is
  bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} debug before any release leg.
- Tracker/oracle: delta 0 on every run (no new resource type; cap_objects covers TcbCap). Every fixture
  that parks a task must resume or terminate_and_drain it in teardown (INV-ADM-6).
- test-history.txt row (append-only, measured TIME) for every class run.

## SIL 3 / SAFETY RISKS

- [S1] **A missed wake site silently voids the stop guarantee.** Mitigation: INV-ADM-1's two layers (the 8
  semantic sites in step 8 with the exact placement rule + the owning refusal backstop with
  adm_wake_backstop_count_ must-read-0), tests 3 and 4, and a DEBUG assertion that no path other than
  admin_resume_err clears admin_parked while writing READY.
- [S1] **Suspending the current/idle/kernel task bricks the CPU.** Mitigation: mechanism refuses self
  (SELF); authority refuses !is_user_ (KERNEL_TASK) and idle (IDLE); order is resolve -> policy ->
  mechanism, every refusal typed with zero side effects.
- [S1] **The resumed PD loses its hang supervision (r1 blocker).** Mitigation: INV-ADM-11 — phase preserved
  and re-armed on resume, wdog_gen bumped on both edges (no stale idle-seam fire), arm-on-park refused,
  kick-after-resume asserted working, paused != revoked observable in procfs without a format change.
- [S1] **A suspended periodic PD is declared overrunning while stopped (the r1 blocker's deadline sibling;
  found this session).** scan_deadlines walks all tasks, so removing the deadline node is not enough.
  Mitigation: INV-ADM-16 skip + the "no deadline_miss while parked" assertion in TEST 8.
- [S1] **Deadline/watchdog kill races the supervisor's stop**, or the reverse (suspension buys unbounded
  budget). Mitigation: suspend removes the deadline node, pauses the scan and screens the watchdog; resume
  shifts both by exactly the elapsed interval (no free budget, no extra period); TEST 8 pins both
  directions plus INV-ADM-14's frozen remaining_ticks/budget.
- [S1] **Silencing a #582 fail-closed.** Mitigation: BUSY refusal via the bounded gs::lockpark_latched_task;
  a halted-in-park task is a legitimate parked target (INV-ADM-9).
- [S1] **Two park owners fighting over one TCB** (debugger park + admin park: a resume clearing admin while
  debug_parked latches, or an attach parking an admin-parked target). Mitigation: enforced mutual
  exclusion in both directions (steps 7 and 12) + TEST 13; documented in scheduler.md.
- [S2] **No production producer for TCB caps at M11** (unchanged from rev 1). Mitigation: TcbCap wrapper +
  documented bind seam; the compose/create producer is a named M12/M14 dependency; QE installs caps
  through the internal factory (test_cap_syscall.cpp:134 pattern).
- [S2] **Authority wider than Q9 (r1 blocker).** Mitigation: INV-ADM-12 — watch-derived authority removed
  entirely, supervisor authority requires a compose-minted SUPERVISOR-scope cap bound to the caller, and
  TEST 14 asserts a self-installed watch confers nothing. Residual, named precisely: the supervisor branch
  has no production mint until M5/M12 (deny-all, fail closed) — identical in status to the TcbCap producer
  the reviewer accepted.
- [S2] **"Never mem-mgr" enforcement gap (r1 blocker).** Mitigation: INV-ADM-15's three layers with
  Untyped-possession as the durable, self-populating layer + the kPdRoleCount == 13 build tripwire + TEST
  15. Residual is an owner decision (Q-r2-b), not a deferral.
- [S2] **Absorbed wake + one-shot handoff** (mutex ownership, semaphore token, queue message handed over
  before the wake). Mitigation: the latch converts "wake" into "pending", resume's READY returns the
  target to the same level-triggered loop; no channel state torn down (INV-ADM-4).
- [S2] **#590 (open, missed-wake).** admin_wake_pending is the only sanctioned missed wake, consumed
  exactly once; #590's fix must preserve the absorb points; TEST 3 is the regression anchor.
- [S2] **The mem-mgr walk takes a foreign CNode leaf lock.** Mitigation: same take class as cap::lookup
  (cap.cpp:202-216) and the foreign-source takes at cap.cpp:509/569/635; the gate runs with no scheduler
  lock held, so no order is created; IRQ contract stays as documented (adds one ledger row, step 19).
- [S3] **#589 (open, rv64 nested-trap).** None by construction: no asm/trap-epilogue change.
- [S3-a] **Closed-set ABI growth** (T-1 §3.1): justified in the same change and posted to #557 before the
  audit (step 18); no number reused; MAX_SYSCALL 99 -> 102; libc drift comment follows.
- [S3-b] **Style/checker gates**: #pragma once, <=80 columns, no single-character names,
  NOLINTBEGIN(bugprone-easily-swappable-parameters) on tcb_control_authorize if the checker fires; the
  five-scalar authority signature is itself swappable-parameter prone — pass a TcbControlAuthority POD
  instead.
- [S3-c] **#582 transition record**: one deferred latch, no new apply site;
  audits/plan-582-transition-compat.md rows 1-9 unaffected, its four follow-ups stay open.
- [S3-d] **Aggregate count drift**: core_task (52), capability (161), proc_elf (131) rows and the aggregate
  membership list (:379-381) are QE's to bump in the same commit as the tests.
- [S3-e] **TCB size vs the pool class**: the 7th latch field may push sizeof(TaskControlBlock) past
  kMemPoolMaxBlockSize on x86_64/riscv64 (8192). The static_assert at task.hpp:1403 is the tripwire; step 1
  names the fallback (pack the two saved-tick fields into a named array) so the coder never guesses.

## OPEN QUESTIONS

- **Q-r2-a (owner, non-blocking): supervisor-cap representation.** Q9 says "supervisor-cap-gated"; the tree
  has no dedicated supervisor capability type (CapType has 16 values, cap_types.hpp:37-57) and no
  supervisor role. Plan takes: the supervisor authority IS the SUPERVISOR-scope TcbCap minted only by
  kernel compose code and bound to (grantee_id, grantee_gen) (INV-ADM-12 branch 6) — Q9-faithful in
  structure, zero object-model growth. Alternative if the owner wants physical separation: a distinct
  CapType::Supervise (spec §4.1 addition, closed-set note in the same change). Decide at M5/M12; either
  way this plan's gate and tests are unchanged apart from the type consulted.
- **Q-r2-b (owner, non-blocking): mem-mgr identity at M4/F10.** This plan ships the enforceable predicate
  (kernel-task refusal + a runtime mem-mgr pid cell that is 0 today + Untyped-possession, which becomes
  true automatically the moment M4 hands the root Untyped to a user PD + the 13-role build tripwire).
  Option O1 (recommended): keep Untyped-possession as the authoritative test. Option O2: at M4, have the
  bootstrap set tcb_control_set_mem_mgr_pid() and treat that cell as authoritative too — both are wired
  into the same one-line predicate, so the choice is a one-line review, not a rewrite.
- **Q-r2-c (owner, non-blocking): watchdog policy on a long park.** INV-ADM-11 preserves the REMAINING
  budget, so a target suspended with 5 ticks left resumes with 5 ticks — deliberately strict. If the owner
  prefers "resume restarts the full period" (kick-equivalent), that is a one-line change in
  admin_resume_err (expiry = now + wdog_period_ticks); TEST 8's doc block records the chosen rule.
