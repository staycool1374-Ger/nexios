# PLAN 590 — rev 2 (rev 1 REJECTED; all 9 findings addressed)

Issue #590 (bug, `kernel:process`, pre-existing; surfaced by the #588 audit r1).
Lane: developer, branch `main`. Production change ⇒ full pipeline (planner →
developer → Phase 3 auditor). Test files (`src/kernel/test/**`,
`test_isolate.*`, harness/class tables) are QE-owned (`testbed`): this plan
hands QE a sketch and writes no test.

**Base tree:** `main` @ `e98296a7cb5a7bc1d4d50ae1d6422c1e62568a69`
(verified: `.git/HEAD` → `refs/heads/main`; identical to the r1 audit base).
Every anchor below was re-read at this commit. Two r1 anchors were stale and are
corrected here: `set_task_ready` is `scheduler_registry.cpp:512-580` (not
`scheduler_queue.cpp:361-381`), and the waitpid task scan is
`scheduler_registry.cpp:816-849` via `task_count()/task_at()`.

## REVISION HISTORY — per-finding resolution

| Finding (design-review-590-r1) | r1 defect | r2 resolution |
|---|---|---|
| [S1] re-verify destroys a successful concurrent wake | undo tested only `state==TERMINATED`, never consumption | **INV-WAIT-6 consumption precedence** (steps 4–5): post-fence `waiting_child_pid==0` ⇒ a wake owns the wait ⇒ never undo, never return −1, never block |
| [S1] progress guarantee false on the trigger path | `release_zombie` unlists the corpse; the scan iterates `all_tasks_` only ⇒ fixed path returns −1 | **second residence** (steps 8–9): `Scheduler::collect_zombie_child()` pops+claims under the zombie leaf; **retention for a waiting parent** (INV-WAIT-5, step 10) keeps the corpse on the list until the owner collects. The QE primary test now targets this path and passes deterministically |
| [S1] unowned post-publish `child` pointer | no lifetime anchor across the window; `reap_orphans` citation insufficient | **no pointer crosses the window** (steps 5,7,9): the re-verify re-resolves **by id** with a fresh scan and the absent case goes to the claimed zombie residence; **INV-WAIT-5** (step 10) plus `kill_declined` (step 11) make the corpse unfreeable by every teardown path while its owner lives |
| [S2] undisclosed 4th death path (`sys_kill` self-fatal) | "exactly three interleavings" was false | **disclosed + analysed + scoped out** (step 16) with a filed follow-up issue spec; folding the wake in is *unsafe* until its missing dequeue/switch-away is fixed (proved in step 16) |
| [S2] seam does not match the #578 precedent; racy single-shot | always-compiled mutable fn ptr + load-and-clear | **CONFIG_DEBUG-gated** header under `src/kernel/test/` + compile-time-gated call site, `__atomic_exchange_n` single-shot, RAII scope with teardown discipline (steps 14–15) |
| [S3] INV-WAIT-3 rejected-alternative proof (c) | wrong mechanism (`admitted` needs IF==1) + non-existent citation | rejection proof **rewritten** on re-verified anchors; the `sync/scheduler_lock_guard.hpp:98-99` citation and proof step (c) are deleted (step 17) |
| [S3] "wake keys on BLOCKED" premise | true at 1 of 3 sites | premise **corrected** (steps 6, 13, 18); the reorder stays safe because pid ⇒ BLOCKED |
| [S3] narrower-hole comment wording | "blocks with pid==0" | corrected to "hlt loop exits on pid==0; the rescan misses ⇒ spurious −1 with lost status" (step 5) |
| [S3] no SEQ_CST codegen precedent | no evidence recorded | **per-arch disassembly evidence** recorded in the audit report at implementation time (step 19) |

Preserved unchanged because the r1 audit verified them sound: the narrowed
sub-window is real; the fence-pair (SB) theory holds on x86-TSO/aarch64/riscv64;
the wake family is exactly 3 implementations with 4 funnel callers; INV-WAIT-4
is harmless to reorder; `kDmesgBase_SCHED + 23` is free; no new TU and no
`nexios_config.h` knob.

## HUNT — anchors re-verified at `e98296a7c`

| Claim | Tree truth (re-read this session) |
|---|---|
| scan / collect / publish / hlt | `syscall_handlers_process.cpp`: scan `76-99`, collect `100-128`, WNOHANG `129-131`, pre-cert `132-147`, publish `148-151` (pid→status→dequeue→BLOCKED, pid **first**), kernel return `152-153`, hlt loop `159-165` |
| wake match / gate | `scheduler_lifecycle.cpp:445-457` (`parent_id` → `find_task` → double `waiting_child_pid` read), `waiter_collects` at `479`, BLOCKED re-queue `480-501`, pid clear `502`, orphan `506-510` |
| funnel order | `scheduler_terminate.cpp`: `dequeue` `126`, `state=TERMINATED` `127`, `wake_waiting_parent` `144`, `release_zombie` `157`; trap twin `253`, `254`, `268`, `272`; both under one `SchedulerLockGuard` (`108`/`251`) |
| release unlists | `release_zombie` `scheduler_lifecycle.cpp:69-113`: `deadline_list_.remove` `77`, `all_tasks_.remove` `78`, `id_table_remove` `79`, `invalidate_pending_switch_to` `87`, zombie push `101-110` |
| free paths (no deferral) | `flush_zombies` `131-197`, `drain_zombie_list` `198-282`, `cleanup_step` `283-333`, `cleanup_step_try` `340-...`; each spares only AP-live (`is_current_on_any_cpu`) and pinned (`MemPool::is_block_pinned`); deferred-kill destroy at `825-915` (`kill_declined` `825`) |
| OR-defer only at reap | `reap_orphans` `593-750`: `can_reap` `637-673` defers a live parented zombie; siblings `drain/flush/cleanup/destroy` do not |
| self-exit retains | `sys_exit` `syscall_handlers_misc.cpp:92-211`: no `release_zombie`; `switch_away` `124`; linkage gate `173`; wake `188-204` (ungated on BLOCKED for the ready); `sys_thread_exit` `syscall_handlers_thread.cpp:158-251`: `167`, `179`, linkage `222-232`, wake `233-245` (no BLOCKED gate) |
| 4th death path | `sys_kill` self-fatal `syscall_handlers_process.cpp:327-337`: `state=TERMINATED` `329`, `exit_code` `330`, `publish_task_death` `334`; **no dequeue, no wake, no release, no switch-away** |
| wake helper primitives | `set_task_ready` `scheduler_registry.cpp:512-580` (wake-while-current → READY + `ready_deferred_` `556-570`); `task_count/task_at` `816-849`; `id_table_find` used lock-free in `kill_declined` `827-828` |
| trap entry masks IRQ | riscv64 `syscall_entry.S:62` `csrci sstatus,2`; aarch64 `vectors.S:112`; x86 syscall gate `hal/idt.cpp:71` `0xEE`; first `sti` at `syscall_handlers_process.cpp:162` |
| dmesg slot | SCHED table `dmesg_catalog.hpp:281-296` uses +16..+22; **+23 free**; SCHED `lookup_severity` arm exists (`343`,`423`) |
| seam precedent | `src/kernel/test/park_seam.hpp:1-65` + `test_park_seam.cpp:9/106/136` (`#if defined(CONFIG_DEBUG) … #else no-ops … #endif`), `ParkSeamScope` RAII, production callers `park_lock.cpp:5`, `scheduler_tick.cpp:48` |

## RETRIEVAL

r1 retrieval is carried forward unchanged (graphify `query_graph` for the wake
family/publish/spec ownership, `get_neighbors(deliver_wait_status)`,
`shortest_path` artifact noted, 5 vault searches; dispositions recorded in
plan-590 r1 §RETRIEVAL and accepted by the r1 audit).

Re-run for the **newly touched areas** (drain/ownership/seam/deferred-kill):

| Query | Result | Disposition |
|---|---|---|
| `graphify query_graph("zombie list drain flush cleanup_step teardown claim ownership free", bfs 3)` | 62 nodes; `drain_zombie_list`(`scheduler.hpp:1177`), `cleanup_step`(:1183), `free`(mempool.hpp:162), **`docs/specs/zombie-list.md`** (community 108) | **used**: the drain's authoritative spec is `docs/specs/zombie-list.md` ⇒ it becomes an affected file (new spare predicate documented there) |
| `graphify query_graph("deferred kill process_deferred_kills kill_declined destroy watchdog", bfs 3)` | `kill_declined()`(`scheduler_lifecycle.cpp:825`), `process_deferred_kills`(`scheduler.hpp:1267`), `destroy`, `terminate`(:766) | **used**: the deferred-kill path frees without `release_zombie` ⇒ it must honour the new ownership rule (step 11) |
| `graphify query_graph("waitpid collect remove_child cleanup delete child reaper reap_orphans parent liveness", bfs 3)` | `reap_orphans`, `waitpid()`, `TaskControlBlock::remove_child()`, `cleanup_and_idle` | **used**: confirms there is no existing "collect by id" API ⇒ the plan adds one (step 9) rather than duplicating #588/#53 logic in the syscall |
| `obsidian_search_vault("waitpid")` | `audits/design-review-588-code-r1.md:19` ("no waitpid/wake section exists" ⇒ header doc-comments are the record) | **used**: INV-WAIT-5/6 documented in `scheduler.hpp` doc-comments + one spec subsection |
| `obsidian_search_vault("park_seam")` | `audits/report-20261118T120828Z-578.md:6,13` (the #578 seam file set incl. `global_state.cpp`, `park_lock.cpp`) | **used**: the seam must be DEBUG-gated and live under `src/kernel/test/` with production accessors — mirrored exactly |
| `obsidian_search_vault("drain_zombie_list")` | `audits/done/semaphore_waiter_teardown_audit-06-08-2026.md:34-36` (`cleanup()` holds no scheduler lock; the drain releases IRQ) | **used**: the collect tail (cleanup+free) must run outside the zombie leaf, exactly like the drain's two-phase tail |
| `obsidian_search_vault("kill_declined deferred kill ownership teardown claim")` | no matches | **ignored** (no prior note; `docs/specs/zombie-list.md` + the #409 claim contract in `task.hpp:1341-1354` are the authority) |

# PLAN: waitpid missed wake — fence pair + consumption precedence + owned corpse (#590)

## AFFECTED FILES
- `src/kernel/syscall/syscall_handlers_process.cpp` — `sys_waitpid` (`59-167`):
  factor the scan predicate; reorder publication (status → dequeue → BLOCKED →
  pid); parent-side fence; seam fire (DEBUG-gated); **consumption-precedence
  re-verify**; absent-branch zombie collect; acquire loads in the hlt loop.
  `sys_kill` (`327-337`): comment only (the 4th death path is named and scoped,
  step 16).
- `src/kernel/task/scheduler.hpp` — `wake_waiting_parent` returns
  `WakeResult` (`:830`); new `collect_zombie_child` declaration; INV-WAIT-3/4/5/6
  contracts on `deliver_wait_status`/`wake_waiting_parent` (`:806-830`);
  `TaskFields` gains `wait_collect_pending` (`:1000-1060`).
- `src/kernel/task/scheduler_lifecycle.cpp` — `wake_waiting_parent` (`445-511`):
  terminator fence, single pid load, **ownership-flag set**, `WakeResult`
  return; the four drain paths (`131-197`, `198-282`, `283-333`, `340-...`):
  **wait-collect spare predicate**; `kill_declined` (`825-829`): flag check;
  new `collect_zombie_child` (next to the drain); seam storage/definitions
  (`#if defined(CONFIG_DEBUG)`).
- `src/kernel/task/scheduler_watchdog.cpp` — the two KILL arms
  (`317-321` deadline `#elif ==3`, `465-469` watchdog `#elif ==3`): when the
  wake reports a collecting parent, dequeue the corpse and skip `defer_kill`.
- `src/kernel/task/scheduler_terminate.cpp` — **not** touched (both call sites
  keep the existing order; the new return value is ignored). Listed here only
  so the auditor confirms the omission is deliberate.
- `src/kernel/syscall/syscall_handlers_misc.cpp` — `sys_exit` inline wake
  (`150-208`): terminator fence at the wake-block entry, single pid load,
  ownership-flag set.
- `src/kernel/syscall/syscall_handlers_thread.cpp` — `sys_thread_exit` inline
  wake (`204-249`): identical three edits.
- `src/kernel/task/task.hpp` — new TCB field `wait_collect_pending_`
  (`:796-797` block) + constructor init (`:408-409`) + doc-comment naming
  INV-WAIT-5 and its two frees.
- `src/kernel/task/scheduler_snapshot_fields.cpp` — capture (`:209`) and
  restore (`:57`) entries for the new field (test-snapshot rewind parity with
  `waiting_child_pid`).
- `src/kernel/log/dmesg_catalog.hpp` — one SCHED record at `+23` (first free
  slot; `:281-296` shows +16..+22 taken) + the `lookup_severity` SCHED arm.
- `docs/specs/scheduler.md` — one INV-WAIT-3/4/5/6 subsection after INV-4b
  (`:146`), marked [IMPLEMENTED] with the #590 reference and the
  `scheduler.md:131-135` divergence note.
- `docs/specs/zombie-list.md` — §4.1/§4.2/§7: document the fifth spare
  predicate ("wait-collect-retained corpse") and its owner-death escape.
- QE-owned surfaces (**sketch only**, §TEST STRATEGY):
  `src/kernel/test/wait_publish_seam.hpp` (new header, DEBUG-gated),
  `src/kernel/test/test_waitpid.cpp`, `tools/meson/qemu_test.py:225`,
  `tools/test-policy.tsv`, `tools/meson/list_tests.py`.
- `tools/meson/gen.mk` — **not** touched: no new `.cpp` (the seam header is
  not a TU; definitions land in an existing TU). Step 20(d) proves it.

## INVARIANTS & CONCURRENCY BOUNDARIES
- **INV-WAIT-1 / INV-WAIT-2 (unchanged):** the range+mapped+writable pre-cert
  (`:138-147`) stays **before** all wait-state publication; a rejected pointer
  leaves no `waiting_child_pid`, no ready-queue mutation, no wake obligation.
  The fix adds no user-pointer access to the wait path except the existing
  `safe_copy_to_user` in the collect tail, reused verbatim.
- **INV-WAIT-3 (unchanged, 4 sites):** `child.state = TERMINATED` is separated
  from that path's read of `parent.waiting_child_pid` by
  `__atomic_thread_fence(__ATOMIC_SEQ_CST)`; the waiter's publish is separated
  from its re-verification read by the same fence. Both fences are required
  (store-buffering/Dekker); release/acquire alone does not forbid both-miss on
  x86-TSO or on armv8/riscv weak models. Sites: `scheduler_lifecycle.cpp`
  (covers all 4 funnel callers), `syscall_handlers_misc.cpp`,
  `syscall_handlers_thread.cpp`, `syscall_handlers_process.cpp`.
- **INV-WAIT-4 (unchanged):** publish order
  `waiting_child_status → dequeue_ready → state=BLOCKED → waiting_child_pid`.
  Premise corrected per S3: only `wake_waiting_parent` re-queues on
  `state==BLOCKED` (`scheduler_lifecycle.cpp:479-480`); `sys_exit` keeps the
  linkage for a BLOCKED parent (`:173`) but readies on `state != TERMINATED`
  (`:188`); `sys_thread_exit` has no BLOCKED gate (`:233-234`). The reorder is
  still safe because **pid ⇒ BLOCKED** (pid is stored last), so every site that
  observes the pid also observes BLOCKED.
- **INV-WAIT-5 (new — corpse ownership for a collecting parent):** every wake
  site sets `child.wait_collect_pending_ = true` at the point where it leaves
  the corpse to the parent. A flagged corpse is **never freed** by
  `flush_zombies`, `drain_zombie_list`, `cleanup_step`, `cleanup_step_try` or
  the deferred-kill `destroy` while `find_task(child.parent_id)` exists and is
  not `TERMINATED`. There are exactly two frees: (i) the owner's waitpid
  collect, (ii) after the owner dies/leaves the tables (the drain's
  owner-death escape, plus the reaper's existing parent-liveness deferral for
  the in-table residence). This is the lifetime anchor the r1 audit demanded;
  it is a *single-writer* flag set by the terminator in its own critical
  section, never by the waiter, so no store-buffering pair is introduced.
- **INV-WAIT-6 (new — consumption precedence):** after the fence, if
  `waiting_child_pid == 0` the publication was consumed by a wake. The waiter
  must then resolve the wait through the normal collect path and **never**
  undo the publication, never return −1, never block. A delivered wake always
  wins over the re-verify.
- **No lock, no lock-order change:** the waiter path still takes no lock (the
  pre-cert is a page-table walk). The wake keeps its three contexts: inside
  `scheduler_lock_` (`scheduler_terminate.cpp:144,268`;
  `scheduler_watchdog.cpp:320,468`) and lock-free (`sys_exit`,
  `sys_thread_exit` — zero `SchedulerLockGuard|IrqGuard` in either TU). The new
  zombie work runs under `arch::IrqGuard` + `zombie_lock_` (leaf) exactly like
  the existing drain; the collect tail (`cleanup()` + free) runs **outside**
  the leaf (two-phase, per `zombie-list.md` §3.3 and
  `audits/done/semaphore_waiter_teardown_audit-06-08-2026.md:34`). No
  `reschedule()` under any lock; no IRQ window widened (everything before the
  first `sti` at `syscall_handlers_process.cpp:162`).
- **Bounded work (§6):** the revive-retry edge is capped by
  `kWaitReverifyRetryMax = 4`; the zombie search is bounded by
  `zombie_count_ + 1` iterations; no allocation, no unbounded loop, no
  new/deletion on the RT path beyond the existing collect tail.
- **Single collect owner:** `sys_waitpid`'s collect block (`:100-128`) remains
  the only place that performs status-store + `thread_join_try_claim` +
  `remove_child` + `cleanup`/free. `collect_zombie_child` only *hands over* a
  claimed corpse and its `{id, exit_code}`; the syscall then runs the same
  tail. #588/#53 logic runs exactly once per corpse.
- **No residence change:** the funnel still calls `release_zombie` exactly as
  today; #523's fault-kill leak fix is preserved because the ownership flag is
  set **only** by a wake that matched a waiting parent (an unwoken corpse is
  freed immediately, as today).
- **Rejected alternative (do not implement) — rewritten per S3:** serializing
  both sides under `scheduler_lock_`. (a) The terminator side must stay
  lock-free: the watchdog/deadline KILL arms run in the on-tick tail that
  `issue #369 T2a` convicted as lock-free (`scheduler_watchdog.cpp:357-364`),
  and `sys_exit`/`sys_thread_exit` publish with `publish_task_death`
  (`scheduler_terminate.cpp:46-67`), which `ENSURE`s that no scheduler lock is
  held (`scheduler_terminate.cpp:50,175-177`). (b) `scheduler_lock_` is
  non-recursive and `terminate_err` asserts depth 0 at entry
  (`scheduler_terminate.cpp:105`, `:248`); a waiter-side take would add a
  reentrancy hazard to a path that is lock-free today (kernel callers of
  `Syscall::handle` may already hold it). (c) It buys nothing the fence pair
  does not give and adds a global-lock WCET on the blocking path. **The r1
  proof step citing the INV-PARK-2 `admitted` discriminator and
  `sync/scheduler_lock_guard.hpp:98-99` is withdrawn** — that file does not
  exist and `admitted` requires `interrupts_enabled()`
  (`core/global_state.cpp:1576-1586`), which is false for the whole publish
  region. Verified neighbours: `sync/park_lock.cpp:184-200`,
  `core/global_state.cpp:1551-1597`.

## STEP-BY-STEP CHANGES

1. `syscall_handlers_process.cpp`, `sys_waitpid`: factor the scan predicate
   (`:77-99`) into two file-local helpers with identical semantics —
   `static TaskControlBlock *find_wait_child(TaskControlBlock &parent, uint64_t target_pid)`
   (wildcard/exact match, skips a thread with `join_waiter_ != nullptr`) and
   `static TaskControlBlock *find_wait_child_by_id(TaskControlBlock &parent, uint64_t child_id)`
   (`parent_id == parent.id && id == child_id`, same join skip). No behaviour
   change; the second helper is the only way the re-verify re-resolves the
   child (never a carried pointer).

2. `sys_waitpid`, publication block (`:148-151`): reorder to
   `cur->waiting_child_status = status_ptr; Scheduler::dequeue_ready(*cur);
   cur->state = TaskState::BLOCKED; cur->waiting_child_pid = target_pid;`
   with the INV-WAIT-4 comment above it (pid last; the wake gate is
   `state==BLOCKED` + pid, and the fence pairs them).

3. `sys_waitpid`, immediately after the pid store and before the
   `!syscall_is_user_task()` return (`:152`): `__atomic_thread_fence(__ATOMIC_SEQ_CST);`
   with the INV-WAIT-3 comment (both fences required; "release/acquire is not
   sufficient here — store buffering"). Placement covers the kernel-caller
   return as well.

4. `sys_waitpid`: declare `uint64_t reverify_retries = 0;` before `for(;;)`.
   After step 3's fence insert, in this order:
   (a) **seam** (DEBUG only, step 14): `kernel::test::fire_wait_publish(*child);`
   (b) **INV-WAIT-6 consumption check**:
   `if (__atomic_load_n(&cur->waiting_child_pid, __ATOMIC_ACQUIRE) != target_pid) continue;`
   — the only other writer of that field is the wake's clear
   (`scheduler_lifecycle.cpp:502`, `syscall_handlers_misc.cpp:166`, `syscall_handlers_thread.cpp:218`), so "field != what I just
   stored" ⇒ a wake consumed the publication. Compare against `target_pid`
   rather than testing for 0 so a pathological `target_pid == 0` wait cannot
   alias the sentinel. **Do not** use `waiting_child_status` as the indicator:
   `deliver_wait_status` clears it on both the delivered and the faulted path
   (`:437-441`), so it cannot distinguish "delivered" from "consumed but not
   delivered". A wake consumed the publication ⇒ do **not** undo; resolution
   happens at the top of the loop (steps 8–9). Comment must state why
   precedence is mandatory: undoing here would turn a delivered wake into −1
   and strand a parented zombie (the r1 S1-a finding).
   (c) **state re-resolve by id**:
   `TaskControlBlock *re = find_wait_child_by_id(*cur, child_id);`
   where `child_id` was captured in the scan iteration (step 1's helper returns
   the TCB; the syscall captures `uint64_t child_id = child->id;` **before**
   the pre-cert at `:138`, i.e. in the same scan epoch as today's
   `child->state` read — the existing exposure class, not widened).

5. `sys_waitpid`, the three outcomes of step 4(c):
   - `re != nullptr && re->state == TERMINATED` ⇒ **undo** (if
     `reverify_retries < kWaitReverifyRetryMax`): `++reverify_retries;`
     `cur->waiting_child_status = nullptr; cur->waiting_child_pid = 0;
     Scheduler::set_task_ready(*cur); continue;` (undo order: status first, pid
     last, so a concurrent wake holding the old slot pointer never sees a
     half-cleared publication).
   - `re != nullptr && re->state != TERMINATED` ⇒ fall through to the normal
     block (hlt loop, `:159-165`).
   - `re == nullptr` ⇒ the corpse left the live tables inside the window; call
     the residence-(b) collect (step 8): if it collects, resolve the wait and
     return the child id; otherwise `log::dmesg_push_sev(SCHED, kDmesgBase_SCHED + 23, WARN, …)`
     once and `return SYS_ERR_GENERIC_FAIL` (−1, no block).
   - Retry-cap exhaustion (only reachable if the rescan keeps observing
     TERMINATED while ownership is lost): log SCHED+23 once and fall through to
     the normal block (**do not** undo — the publication must stay intact for a
     real wake).
   Comment wording must be exact per S3: the pre-INV-WAIT-4 hole made the hlt
   loop exit on `pid == 0` and the rescan miss the orphaned child — "spurious
   −1 with a lost status", **not** an indefinite block.

6. Terminator side, three wake sites — same three edits at
   `scheduler_lifecycle.cpp:445-457`, `syscall_handlers_misc.cpp:150-158`,
   `syscall_handlers_thread.cpp:204-211`:
   (a) `__atomic_thread_fence(__ATOMIC_SEQ_CST);` as the first statement of the
   wake block (in `wake_waiting_parent`: directly after the `ENSURE` at `:446`,
   before `child.parent_id` is used);
   (b) replace the double read of `waiting_child_pid` with one
   `const uint64_t waited = p->waiting_child_pid;` used by both comparisons
   (single observation ⇒ no half-matched comparison);
   (c) at the point where the corpse is left to the parent (after
   `deliver_wait_status`), set `child.wait_collect_pending_ = true;`
   (`__atomic_store_n(&child.wait_collect_pending_, true, __ATOMIC_RELEASE);`)
   with the INV-WAIT-5 comment: "the parent owns this corpse; no zombie path
   and no deferred kill may free it while the owner lives". Set it
   unconditionally at the match point — an orphaned corpse (`parent_id == 0`)
   is freed by the owner-death escape anyway, so no branch is needed.

7. `scheduler.hpp` / `scheduler_lifecycle.cpp`: give the wake a return type so
   the watchdog can act on it —
   `enum class WakeResult : uint8_t { NO_WAITER = 0, WOKE_NOT_COLLECTING = 1, PARENT_COLLECTS = 2 };`
   `static WakeResult wake_waiting_parent(TaskControlBlock &child) noexcept;`
   (not `[[nodiscard]]`; the funnel call sites ignore it). Return
   `PARENT_COLLECTS` when the matched parent is `BLOCKED`
   (`waiter_collects == true`), `WOKE_NOT_COLLECTING` when a parent was
   readied without collection, `NO_WAITER` otherwise.

8. `scheduler_lifecycle.cpp`, new `Scheduler::collect_zombie_child`
   (placed immediately after `drain_zombie_list`):
   `static bool collect_zombie_child(TaskControlBlock &parent, uint64_t target_pid, uint64_t &out_child_id, uint64_t &out_exit_code) noexcept;`
   Body: `arch::IrqGuard` + `SpinLockGuard<sync::SpinLock> zguard(zombie_lock_)`;
   walk `zombie_head_` with a predecessor pointer, bounded by
   `__atomic_load_n(&zombie_count_, __ATOMIC_RELAXED) + 1`; match
   `t->magic == TaskControlBlock::TCB_MAGIC && t->parent_id == parent.id &&
   (target_pid == (uint64_t)-1 || t->id == target_pid)`; skip
   `MemPool::is_block_pinned(t)`; unlink under the leaf
   (`prev ? prev->zombie_next_ : zombie_head_ = t->zombie_next_`, fix
   `zombie_tail_`, `__atomic_sub_fetch(&zombie_count_, 1, RELAXED)`,
   `t->zombie_next_ = nullptr`); `TaskControlBlock::try_claim_teardown(t)`
   **inside the leaf** (atomic CAS; on failure re-link at the tail and report
   not-collected). Outside the leaf: `parent.remove_child(t)`,
   `t->cleanup()`, and `if (!MemPool::is_block_pinned(t)) MemPool::free(t);`
   — the exact owner tail of `drain_zombie_list` (`:277-280`), keeping the
   claim (we own it). Fill the out-params from the TCB before the free.
   Doc-comment: "atomic pop+claim; the claim is taken under the leaf so a
   concurrent drain either claims first (we never see the entry) or defers to
   us — never both".

9. `sys_waitpid`, the absent branch (`:100-101`) — insert **before** the
   `return -1`:
   `uint64_t zid = 0, zcode = 0;
   if (Scheduler::collect_zombie_child(*cur, target_pid, zid, zcode)) {
       if (status_ptr) { /* same body as :111-121: safe_copy_to_user for user
                            tasks, direct store for kernel callers */ }
       return zid;
   }`
   Then the existing `return SYS_ERR_GENERIC_FAIL` (WNOHANG ordering at
   `:129-131` unchanged). Step 5's `re == nullptr` case reuses this same call
   (`return zid` on success, SCHED+23 WARN otherwise).

10. `scheduler_lifecycle.cpp`, four drain paths — add the fifth spare
    predicate next to the two existing ones, with the INV-WAIT-5 comment and
    the `docs/specs/zombie-list.md` cross-reference:
    `static bool wait_collect_spared(const TaskControlBlock &t) noexcept` —
    `if (!t.wait_collect_pending_) return false;` → `auto *p = Scheduler::find_task(t.parent_id);`
    (lock-free id-table read; precedent `kill_declined` `:827-828`) →
    `return p != nullptr && p->state != TaskState::TERMINATED;`
    - `flush_zombies` (`:137-196`): spare like the AP-live/pinned legs —
      re-queue at the tail, `continue`, counted inside the `max_flush` budget.
    - `drain_zombie_list` (`:209-282`): same rotation, charged to
      `spared`/`pass_budget` (`:206-208`).
    - `cleanup_step` (`:289-325`): leave it listed and return (exact
      AP-live/pinned shape at `:306-312`).
    - `cleanup_step_try` (`:340-...`): same as `cleanup_step`.
    No other change to the drain; the claim/cleanup tail stays where it is.

11. `scheduler_lifecycle.cpp`, `kill_declined` (`:825-829`): add
    `|| task->wait_collect_pending_` with a comment that the owning parent
    completes the teardown (matches the existing "Dropped, not retained: the
    owner completes the tail" contract at `:879-886`).

12. `scheduler_watchdog.cpp`, both KILL arms (`:317-321`,
    `:465-469`): replace the bare `Scheduler::wake_waiting_parent(task);` with
    `if (Scheduler::wake_waiting_parent(task) == Scheduler::WakeResult::PARENT_COLLECTS) { Scheduler::dequeue_ready(task); return; } /* else: */ Scheduler::defer_kill(&task);`
    — the corpse stays parented/in-table for the parent's rescan (no
    TERMINATED node may remain in a ready queue: INV-2), and the deferred kill
    is not armed. Verify `dequeue_ready`'s locking contract at implementation:
    if it needs the global lock, wrap the call in `SchedulerLockGuard`
    (precedent: `destroy()` takes it per item from the same tail,
    `:856-861`). Compile-time gated arms (`CONFIG_DEADLINE_ACTION==3`,
    `CONFIG_WATCHDOG_ACTION==3`): the edit must also build under the other
    `#elif` selections (no unused-variable warnings).

13. `task.hpp` + `scheduler_snapshot_fields.cpp` + `scheduler.hpp` docs: add
    `bool wait_collect_pending_;` after `waiting_child_status`
    (`task.hpp:796-797`), init `false` in the constructor list
    (`:408-409`); add it to `TaskFields` (`scheduler.hpp:1020-1021` block) and
    to `capture_task_fields` (`scheduler_snapshot_fields.cpp:209`) and
    `restore_task_fields` (`:57`) beside `waiting_child_pid`. Doc-comment
    states INV-WAIT-5 and its two frees.

14. Seam (QE-owned header + dev-owned definitions, #578 shape):
    (a) QE creates `src/kernel/test/wait_publish_seam.hpp` containing
    `#if defined(CONFIG_DEBUG)` declarations `void arm_wait_publish(TaskControlBlock &);`
    `void fire_wait_publish(TaskControlBlock &) noexcept;`
    `class WaitPublishSeamScope { … armed(); ~WaitPublishSeamScope(); }`
    and an `#else` half with inline no-ops (`fire` → empty, `armed` → false) —
    byte-for-byte the `park_seam.hpp:41-63` contract, including "no production
    caller ever arms it" and "all bodies compile out under release".
    (b) Dev implements the definitions in `scheduler_lifecycle.cpp` under
    `#if defined(CONFIG_DEBUG)`: one TU-local
    `static void (*s_wait_publish_hook)(TaskControlBlock &) = nullptr;`,
    `fire_wait_publish` performs **`__atomic_exchange_n(&s_wait_publish_hook, nullptr, __ATOMIC_ACQ_REL)`**
    and invokes only a non-null result (single-shot; two concurrent publishers
    cannot both fire), and `WaitPublishSeamScope` clears the hook + asserts
    `irq_guard_depth` assumptions in its destructor. The syscall call site
    (step 4a) is wrapped in `#if defined(CONFIG_DEBUG)` so release has **zero**
    load, zero indirect call, and no dangling pointer.
    (c) Contract comment: debug-only; UP-only (no SMP test may arm it); one
    armer; the scope is the teardown discipline; a re-entrant wait cannot
    re-fire.

15. `dmesg_catalog.hpp`: append
    `{ErrorSubsystem::SCHED, kDmesgBase_SCHED + 23, LogSeverity::WARN, "<text>"}`
    to the SCHED table (`:271-296`) and the matching `lookup_severity` arm;
    text covers both uses (retry-cap exhaustion and unrecoverable wait
    resolution). Re-verify `+23` unclaimed with
    `grep -n 'kDmesgBase_SCHED' src/kernel` (currently +1, +14..+22).

16. 4th death path (`sys_kill` self-fatal, `syscall_handlers_process.cpp:327-337`):
    **disclose, analyse, scope out.** Analysis to be written as the comment:
    (i) the leg sets `TERMINATED` + `exit_code` + `publish_task_death` and calls
    **no wake**, so a waitpid parent is *never* readied — the identical
    lost-wake outcome, deterministic rather than racy;
    (ii) it also does **no dequeue and no `switch_away_from_terminating`**, so
    the corpse can still be current on this CPU when the parent (if woken by
    any other means) collects and frees it — adding only the wake here would
    introduce a cross-CPU use-after-free of a live stack. Therefore the fix is
    *not* a one-line wake; it must fix the self-kill residence first.
    Action: file the follow-up issue **before** implementing, with this spec:
    "self-fatal signal kill leaves the task current: dequeue + publish the
    deferred-switch arm (`switch_away_from_terminating`) before the trap
    return, then call `wake_waiting_parent` (WakeResult consumed as in step 12);
    add `smp`/`process_waitpid` coverage for kill-self + parent-waitpid".
    Cross-reference the issue number in the `sys_kill` comment and in
    `docs/specs/scheduler.md` (step 18). `daemon_mgr.cpp:462` stays disclosed
    and scoped out exactly as in r1 (no wake there either — same follow-up
    class, listed in the same issue).

17. Docs, two places, same text: (a) `scheduler.hpp` doc-comments
    (`:806-830`): INV-WAIT-3 (fence pair, both sides), INV-WAIT-4 (order +
    corrected gating premise), INV-WAIT-5 (ownership flag + two frees),
    INV-WAIT-6 (consumption precedence); (b) `docs/specs/scheduler.md`: one
    subsection after INV-4b (`:146`) marked [IMPLEMENTED], naming the four
    fence sites, the deliberate divergence from the locked post-publish
    rechecks at `:131-135`, and the scoped-out 4th death path with its issue
    number. (c) `docs/specs/zombie-list.md` §4.1/§4.2/§7: the fifth spare
    predicate, the owner-death escape, and the "forced drain does not override
    it" statement (the predicate applies uniformly in all four paths).

18. Verification greps / static checks (paste into the issue):
    (a) `grep -n '__atomic_thread_fence' src/kernel/task src/kernel/syscall`
    ⇒ exactly 4 sites; (b) `grep -n waiting_child_pid` ⇒ the 3 wake sites +
    the hlt loop + `shell.cpp:2209` diagnostic only (no new reader);
    (c) `grep -n wait_collect_pending_` ⇒ task.hpp, scheduler.hpp (TaskFields),
    scheduler_lifecycle.cpp (set + 4 spare + kill_declined), `sys_exit`,
    `sys_thread_exit`, scheduler_snapshot_fields.cpp (2), test files —
    i.e. writer set = the 3 wake sites, reader set = the enumerated paths, and
    **no reader in the syscall TUs**; (d) `git status --porcelain` shows no
    `gen.mk`/`.cpp` add (no new TU); (e) `graphify update .` after the edit.

19. SIL-3 codegen evidence (new per S3-4): after the build, disassemble the
    four fence sites on all three arches
    (`objdump -d build/meson-*/src/kernel-*.elf`) and record `mfence` (x86_64),
    `dmb ish` (aarch64), `fence iorw,iorw` (riscv64) in the audit report,
    noting that the tree's only other `__atomic_thread_fence` uses are
    RELEASE/ACQUIRE (`vfs_srv.cpp`, `block_srv.cpp`) so SEQ_CST is a new
    codegen class. Cross-arch parity: the emitters are shared C++; aarch64 and
    riscv64 must build+link and run their arch classes (step §TEST STRATEGY).

20. Cross-arch parity (per project rule): the change is arch-independent C++,
    no asm edit. Gate: `x86_64`, `aarch64`, `riscv64` debug builds green with
    the waitpid classes selected; `arch_aarch64`/`arch_riscv64` link+select
    recorded; no `nexios_config.h` change (verified by `git diff`).

## TEST STRATEGY
QE owns every item (branch `testbed`). Stub-first: land `JARVIS_TEST_PASS()`
stubs, then fill. The seam is UP-only and DEBUG-only (step 14c).

- **`waitpid_missed_wake_consumed_collects`** (primary, `test_waitpid.cpp`,
  class `process_waitpid`, UP-deterministic). Parent kernel task (prio 20),
  child prio 11, `parent->add_child(child)`. Arm `WaitPublishSeamScope` with a
  hook that reproduces **the full cross-CPU terminator inside the publication
  window**: `Scheduler::dequeue_ready(*child); child->state = TERMINATED;
  child->exit_code = 42; (void)Scheduler::wake_waiting_parent(*child);
  Scheduler::release_zombie(*child);`. Then `Syscall::handle(WAITPID, child_id,
  &status, 0, …)`. Asserts: return `== child_id` (**not** −1), `status == 42`,
  `parent->waiting_child_pid == 0`, `parent->waiting_child_status == nullptr`,
  `parent->state != BLOCKED`, `Scheduler::find_task(child_id) == nullptr`,
  `Scheduler::snapshot_zombies(...) == 0`, ResourceTracker delta 0. Teardown
  `ScopeGuard`: disarm the seam, `Scheduler::drain_zombie_list()`,
  `Scheduler::set_task_ready(parent)` so a pre-fix failure cannot wedge the
  suite. *This test exercises steps 4b/5 (consumption precedence) + 8/9 (the
  released-corpse collect) together — the r1 sketch's assertion now holds.*
- **`waitpid_released_corpse_collects`** (S1-b direct): same hook but **the
  funnel minus the wake** (`dequeue`, `TERMINATED`, `exit_code = 42`,
  `release_zombie`; no `wake_waiting_parent`) — i.e. the miss leg. Asserts the
  same outcome via the absent branch (step 9). Documented limitation: UP only;
  on SMP a concurrent drainer may win the race (see SIL risks, R-1).
- **`waitpid_retained_corpse_survives_drain`** (INV-WAIT-5): arm the hook to
  terminate + wake + release, but stop before the collect by calling the hook
  and then `Scheduler::drain_zombie_list()` *from the hook* → assert
  `snapshot_zombies` still returns 1 while the owner is alive; then collect via
  waitpid and assert 0; second leg: terminate the owner first, drain, assert
  the corpse is freed (owner-death escape).
- **`waitpid_reverify_wildcard_collects`**: same as the primary with
  `target = (uint64_t)-1`.
- **`waitpid_reverify_never_fights_the_wake`**: hook armed with a read-only
  no-op; a second task terminates the child; assert the classic wake path
  resolves exactly once and the dmesg ring holds **no** SCHED+23 record.
- **`waitpid_publish_order_preserved`**: at the seam point (read-only) assert
  `parent->state == BLOCKED`, `!parent->in_ready_queue_`,
  `waiting_child_pid == target` — pins INV-WAIT-4/INV-2.
- **`waitpid_zombie_claim_is_atomic`**: unit-level — a claimed zombie is
  skipped by `flush_zombies`/`cleanup_step` while claimed and freed after the
  claim is released by the collect (covers the two-phase tail).
- **New SMP class `smp_waitpid`** (x86_64 only, like `smp`/`smp_multicpu`:
  `tools/test-policy.tsv` `class|smp_waitpid|arch_scope|x86_64`;
  `tools/meson/qemu_test.py:225` add to the `-smp 2` tuple; register in
  `list_tests.py`): N≈200 loop, parent CPU0, child pinned CPU1 via
  `set_affinity`, child self-terminates after a varying spin, parent waits;
  assert return `== child id`, `status` correct, parent not stranded;
  `ScopeGuard` repair. Timing supplement only (never replaces the
  deterministic tests).
- **A/B (red) evidence:** the seam does not exist on `main`, so "red on
  unmodified main" is produced by cherry-picking the *test* onto a scratch
  branch whose *production* re-verify/collect edits are reverted (a local,
  uncommitted revert) and recording the failing run (returns −1, parent left
  BLOCKED) on the issue. No committed A/B knob (CODING_STYLE §6: no
  debug/release behavioural divergence).
- **Existing classes to run:** `process_waitpid` (primary TU),
  `scheduler_core` (`wake_waiting_parent` + kernel waitpid), `deadline`
  (`CONFIG_DEADLINE_ACTION==3` arm + watchdog KILL arms — the paths edited in
  step 12), `task_lifecycle` + `cleanup` + `cap_pager` (**#523 regression
  guard**: an unwoken fault-killed child must still be freed promptly),
  `smp`/`smp_sched`/`smp_multicpu` + `smp_waitpid` (x86_64), `core`
  (aggregate), `selftest`, release `all`, and `arch_aarch64`/`arch_riscv64`
  (shared C++ only; step 19 records the fence codegen).

## SIL 3 / SAFETY RISKS
- [S1] **A lost wake parks a user-trap waiter indefinitely.** Mitigated by the
  fence pair (INV-WAIT-3) + consumption precedence (INV-WAIT-6) + the owned
  corpse (INV-WAIT-5); the A/B run is the evidence.
- [S1] **Dropping either fence silently reinstates the defect** (both are
  individually invisible in review). Mitigated by one named contract in three
  code sites + header doc + grep step 18(a) (fence count must be exactly 4) +
  per-arch codegen evidence (step 19).
- [S1] **R-1 (disclosed residual): the terminator missed the publication *and*
  a concurrent drainer reclaimed the corpse before the waiter's absent-branch
  collect.** Outcome: −1 with a lost exit status — the pre-existing #523
  release-policy defect, *not* a hang, leak or UAF (the collect is
  pop+claim-atomic, so it either gets the corpse or sees nothing). Probability
  is limited to the ~100-instruction window between the waiter's scan and its
  publish on a funnel death; closed deterministically only by unconditional
  POSIX retention, which #523's fault-kill leak fix forbids. Mitigation:
  SCHED+23 WARN on the path, test coverage of the leg (UP-deterministic), and
  the follow-up issue (step 16 file, same issue family) that retires the
  in-funnel release in favour of #221's residence with a bounded reaper rule.
- [S1] **Invariant-5 enforcement is spread over 5 free paths + 1 predicate.**
  A future free path that forgets the predicate reintroduces a use-after-free
  of an owned corpse. Mitigation: the predicate lives in one helper
  (`wait_collect_spared`), all four drain paths plus `kill_declined` call it,
  and step 18(c) enumerates the reader set; `docs/specs/zombie-list.md` §7
  becomes the normative list.
- [S1] **The drain's owner-liveness read is lock-free** (`find_task` under the
  zombie leaf). A stale "absent" verdict frees a flagged corpse; the waiter
  then takes R-1 (never a UAF, because both the drain and the collect claim
  under the same leaf). Disclosed in the predicate's comment; precedent is
  `kill_declined` (same read, same context class).
- [S2] **Head-of-line stall in `cleanup_step`** when the head is an owned
  corpse: the idle drain makes no progress on that list head (it still frees
  nothing else from the head) until the owner collects. Bounded by the wake
  guarantee (the owner is READY and running) and by `flush_zombies`'
  rotate-and-free-others behavior. Documented in §7 of the zombie-list spec;
  if QE sees a reclaim-rate regression, the follow-up issue carries the
  bounded-grace alternative (defer only N ticks).
- [S2] **Wrong undo order** (pid before status) would let a concurrent wake
  deliver into a dropped slot pointer. Mitigated by the fixed undo order
  (step 5) and the explicit comment.
- [S2] **An over-eager re-verify** (full re-scan instead of the selected child)
  livelocks on the wildcard-with-live-sibling shape. Mitigated by
  `find_wait_child_by_id` only + `kWaitReverifyRetryMax` + the
  progress-by-proof comment.
- [S2] **The undo path readies the current task** (`set_task_ready` on
  `BLOCKED`-current → `READY` + `ready_deferred_`,
  `scheduler_registry.cpp:556-570`). This is the same shape the wake already
  produces for a self-targeted wake; the publish-order test pins it.
- [S2] **`dequeue_ready` from the watchdog ISR tail** (step 12) may need the
  global lock. Mitigated by the explicit verification step and the
  `SchedulerLockGuard` fallback (precedent: `destroy()` per item from the same
  tail).
- [S2] **The scoped-out 4th death path** (self-fatal kill) keeps a deterministic
  hang for a waiting parent until the follow-up lands. Disclosed in code +
  spec + the filed issue; it is *not* made worse by this change (r1's case
  analysis is corrected).
- [S3] `waiting_child_pid`/`wait_collect_pending_` remain plain fields
  observed cross-CPU under a fence/release contract (no `__atomic`
  conversion): aligned scalars, no tearing, same discipline as `state`. Do not
  mix the two approaches.
- [S3] Wildcard first-match masking (`:93-97`, no state preference) is
  unchanged and still scoped out (latency/wrong-order, not a hang).
- [S3] The seam is DEBUG-only; a release build must be behaviourally identical
  (step 14b compile-time gate makes this structural, not conventional).

## OPEN QUESTIONS
- Seam ownership ordering is the only item that blocks start: QE must land
  `src/kernel/test/wait_publish_seam.hpp` (step 14a) **before** the dev's
  DEBUG-gated call site compiles. Confirm QE-first same-cycle landing, or grant
  the dev a one-file ownership exception for that header (the #578 cycle landed
  header+definitions together).
