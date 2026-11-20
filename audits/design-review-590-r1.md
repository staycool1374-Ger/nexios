# AUDIT REPORT 2026-11-19 design-review-590-r1
PATCH: n/a (design audit of audits/plan-590.md against tree at e98296a7c, branch main)
FILES: src/kernel/syscall/syscall_handlers_process.cpp, src/kernel/task/scheduler_lifecycle.cpp, src/kernel/syscall/syscall_handlers_misc.cpp, src/kernel/syscall/syscall_handlers_thread.cpp, src/kernel/task/scheduler.hpp, docs/specs/scheduler.md, src/kernel/log/dmesg_catalog.hpp

## VERIFIED SOUND (not findings)
- Publish order today is pid-first: `syscall_handlers_process.cpp:148` pid,
  `:149` status, `:150` dequeue, `:151` BLOCKED. The narrower sub-window
  (wake lands after pid store, before BLOCKED) is REAL: `wake_waiting_parent`
  (`scheduler_lifecycle.cpp:455-457,479`) sees pid-match but
  `waiter_collects==false`, so it skips the enqueue AND clears pid (`:502`).
- Fence-pair theory is sound in isolation: waiter W(pid)+FENCE+R(state) vs
  terminator W(state)+FENCE+R(pid) is the SB litmus; release/acquire alone
  does NOT forbid both-miss (correct on x86 TSO and on armv8/riscv weak
  models). `__atomic_thread_fence(SEQ_CST)` is the correct primitive class.
- Wake implementations are exactly 3 (`wake_waiting_parent`,
  `sys_exit` inline `:150-208`, `sys_thread_exit` inline `:204-249`); the 4
  funnel callers (`scheduler_terminate.cpp:144,268`,
  `scheduler_watchdog.cpp:320,468`) all route through `wake_waiting_parent`,
  so one fence there covers them. `sys_exit`/`sys_thread_exit` take no lock
  (0 `SchedulerLockGuard|IrqGuard` matches in either TU) — verified.
- Trap-entry masking on all three archs verified
  (`riscv64/syscall_entry.S:62`, `aarch64/vectors.S:112`,
  `x86_64/hal/idt.cpp:69-71` 0xEE interrupt gate); first `sti` is at
  `syscall_handlers_process.cpp:162`, after publication — no same-CPU
  preemption in the window. SMP cross-CPU is the only trigger. Verified.
- INV-WAIT-4 reorder harms no existing reader: shell diagnostic
  (`services/shell.cpp:2209`) is read-only; F4 absorb keys on `admin_parked`
  (`scheduler_suspend.cpp:96-115`), not pid; #582 reap deferral keys on
  parent-liveness, not order. `send_sync` divergence rationale (locked vs
  lock-free publish) is accurate (`scheduler.md:131-135`). Verified.
- Wildcard first-match masking (`:93-97`, no state preference) and
  `daemon_mgr.cpp:462` no-wake kill are real and correctly scoped OUT. Verified.
- SCHED+23 free (`dmesg_catalog.hpp:281-296` shows +16..+22 taken; SCHED arm
  exists at `:343,423`). No new TU, no `nexios_config.h` knob — verified.

## FINDINGS
- [S1] plan step 4 (`syscall_handlers_process.cpp` re-verify) — re-verify destroys a successful concurrent wake
  WHY: The undo condition checks only `child->state == TERMINATED`, not whether the publication is still outstanding. If the terminator's wake lands between the waiter's fence and re-verify (fully legal SMP interleaving: wake delivers status via `deliver_wait_status`, patches the trap frame, enqueues the parent, clears pid), the re-verify still sees TERMINATED, performs undo (`set_task_ready` + `continue`), and the top-of-loop scan misses the released child (see S1 below) — a delivered wake is converted into return -1 plus a leaked parented zombie. Two consumers (wake + re-verify) share one publication with visibility-ordering only, no consumption check.
- [S1] plan step 5 (`syscall_handlers_process.cpp` case analysis) — progress guarantee false on the trigger path itself
  WHY: Every cross-CPU funnel death (`terminate_err` at `scheduler_terminate.cpp:126-157`, `terminate_err_from_trap` at `:253-272`, watchdog/deadline KILLs) calls `release_zombie`, which removes the child from `all_tasks_` and the id table (`scheduler_lifecycle.cpp:77-79`) — but the waitpid scan iterates `all_tasks_` only (`scheduler_registry.cpp:816-849`). After undo+`continue`, the scan CANNOT reselect the child: it returns -1, the parented zombie leaks (reap defers while the parent lives, `:646-667`), and the exit status is lost. "The scan must find the same TERMINATED child and collect it" and "the retry cap is unreachable by proof" hold only for self-exit deaths (which never release); they fail exactly on the SMP cross-CPU path the plan names as the trigger. The sketched primary test arms a hook calling `release_zombie` and then asserts `return == child id, status == 42` — it returns -1 with the fix applied, so the A/B evidence claim is false as written.
- [S1] plan steps 4-5 (`syscall_handlers_process.cpp`) — post-publish `child` dereference has no lifetime anchor
  WHY: The re-verify reads `child->state` after publication, but the pointer is unowned: once the terminator orphans (wake-miss leg, `scheduler_lifecycle.cpp:506-510`) or releases the child, `drain_zombie_list` (`:198-282`), `flush_zombies` (`:131-197`) and `cleanup_step` (`:283-333`) free it with NO parented-zombie deferral — the plan's "pointer stays owned" cites only `reap_orphans` (`:637-639,:657-662`), which is the one drain path that defers. The fix therefore introduces a cross-CPU use-after-free read (freed MemPool block reuse can also forge either branch outcome). Narrow window, but SIL 3 requires safety under all interleavings.
- [S2] plan §HUNT/steps 2-5 (`syscall_handlers_process.cpp:327-337`) — undisclosed non-waking death path breaks the "exactly three interleavings" case analysis
  WHY: `sys_kill` self-fatal sets `TERMINATED` + `publish_task_death` with NO `wake_waiting_parent` call (unlike every other death path). A post-publication self-kill hangs a waitpid waiter even with all 4 fences placed, and no pid read ever occurs there — the case analysis assumption "the terminator's pid read is ordered after the waiter's publish" is false for this path. (`daemon_mgr.cpp:462` is disclosed; this one is not; boot-time `taskdefs.cpp:658` is benign — no waiters exist pre-daemon-boot.)
- [S2] plan step 7 (seam precedent claim) — does not match the cited #578 precedent and the single-shot arm races
  WHY: The #578 seam (`src/kernel/test/park_seam.hpp:11-12`) compiles OUT under release (CONFIG_DEBUG-gated waiver); the proposed `s_wait_publish_hook` is always-compiled with a mutable production function pointer plus indirect call — a deliberate divergence the plan presents as precedent compliance. Additionally the load-and-clear single-shot is non-atomic: two concurrent publishers (SMP, two waiters) can both observe non-null and double-fire; needs an atomic exchange or a documented UP-only contract plus teardown discipline in the contract comment.
- [S3] plan INV-WAIT-3 rejected-alternative proof (c) — wrong mechanism and wrong citation
  WHY: The INV-PARK-2 `admitted` discriminator requires `interrupts_enabled()` (`core/global_state.cpp:1576-1586`); the whole publish region runs IRQ-masked (verified above), so a lock release there could not newly arm the park latch. The cited `sync/scheduler_lock_guard.hpp:98-99` does not exist (actual: `sync/park_lock.cpp:184-200`, `core/global_state.cpp:1551-1597`). Conclusion (reject the lock) still stands via lock-free ISR/watchdog contexts + non-recursive lock, but proof step (c) is unsound.
- [S3] plan step 2/INV-WAIT-4 (`scheduler_lifecycle.cpp:479`) — "wake keys on BLOCKED" is true of only one of three sites
  WHY: `sys_thread_exit` (`syscall_handlers_thread.cpp:233-234`) wakes with `set_task_ready` whenever the parent is not TERMINATED — no BLOCKED gate at all (`sys_exit` gates at `:173`; only `wake_waiting_parent` gates at `:479`). The reorder remains safe (pid then implies BLOCKED), but the stated current-gating premise is inaccurate for one site.
- [S3] plan step 5 comment wording — narrower-hole outcome is -1, not "blocks with pid==0"
  WHY: With pid cleared pre-BLOCKED, the hlt loop (`syscall_handlers_process.cpp:160-161`) exits immediately on `pid==0` and the rescan misses the orphaned child — spurious -1 with lost status, not an indefinite block. Comment accuracy only.
- [S3] plan steps 3/8/9 — zero SEQ_CST codegen precedent in tree
  WHY: The only `__atomic_thread_fence` uses are RELEASE/ACQUIRE (`vfs_srv.cpp`, `block_srv.cpp`); no SEQ_CST fence exists anywhere (`grep SEQ_CST src/kernel` = plan file only). Compiler lowering (mfence / dmb ish / fence) is expected-correct, but SIL 3 evidence should record per-arch codegen (disassembly) for the 4 new sites at implementation time.

DECISION: REJECTED
