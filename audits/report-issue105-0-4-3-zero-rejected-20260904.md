# SIL 3 Audit Report — issue #105 Part B (Crash Supervisor / DeathNotify)

- **Auditor:** independent SIL 3 safety auditor
- **UTC timestamp:** 2026-09-04 07:11:36Z
- **Branch:** main (production development)
- **Patch under audit:** `audits/pending_patch.diff` (git diff vs main, 14 files)
- **Scope:** async task-death notification registry (`DeathNotify`) + `SYS_DEATH_WATCH/RECV/UNWATCH` (66–68) + `cleanup()` hooks + `cap_death` test class (9 tests)
- **Context claims verified:** debug gate 1044/1044 (registered 1059), release 85/85, check-style Errors 0 — confirmed `make check-style` = Errors 0; `git apply --check` on the rejected patch = clean.

## FINDINGS

### S1 — Safety/correctness violation: NONE

The poke path was examined as an S1 candidate (check 4). The freed-supervisor
use-after-free is **not** classified S1 because the barrier chain is effective:
`terminate()` removes the target from `id_table_` synchronously under
`scheduler_lock_` (`release_zombie` → `id_table_remove`, scheduler.cpp:433-459),
so `Scheduler::find_task` (lock-free id-table probe, scheduler.cpp:1125-1134)
cannot return a task that has already been terminated; a reaped TCB is
0xDD-poisoned (`MemPool::free`, mempool.cpp:139/425) so `task_live()`'s magic
check rejects it; and `Notify::notify()` further guards a TERMINATED/REAPED
waiter plus generation. The residual window (terminate **and** reap between
`task_live()` and `sup->notify.notify()`, which would need a multi-context-switch
preemption chain within a 3-instruction window) is real but practically
unreachable on this single-core deferred-switch kernel. It is captured as S3-1
(the spec's poke-safety rationale is overstated) rather than S1.

### S2-1 — ResourceTracker `death_watches` is never verified (check 10)

`ResourceCounters` gains `death_watches`, and `track_death_watch_add/remove`
are wired, but `ResourceTracker::check()`'s `any_leak()`
(resource_tracker.cpp:78-108) compares **13 fields and omits `death_watches`**.
Consequences:

- The design-doc invariant #5 ("ResourceTracker `death_watches` is zero-delta
  across every test") and the test-file claim ("ResourceTracker death_watches
  must be zero-delta") are **not enforced** — the counter is decorative.
- `track_death_watch_reset()` in `snapshot_reset()` (death_notify.cpp:370)
  masks any leak; and because `snapshot_reset()` runs in the "Post-reload
  fixup" block (test_isolate.cpp:1364) which executes **after** the leak check
  (test_isolate.cpp:721), a test that leaves PENDING slots passes today.
- Direct proof: `death_watch_registry_full_fails_closed` ends with **kMax
  unconsumed PENDING records** (helpers[1..kMax-1] + `extra` all terminated,
  records latched, never drained). If `death_watches` were compared, that test
  would fail with `baseline+kMax` vs `baseline`. It passes only because the
  field is not checked.

**Fix (included in rejected_patch.diff):** add `if (current.death_watches >
baseline.death_watches) return true;` to `any_leak()` and drain the leftover
PENDING records in the registry-full test so the counter is genuinely
zero-delta.

### S2-2 — `SYS_DEATH_RECV` consumes the record before validating the output pointer (check 6)

`sys_death_recv` (syscall_handlers_death.cpp:46-72) calls `DeathNotify::recv()`
**first** (which latches the slot PENDING→FREE and decrements the tracker under
`s_lock_`), and only then validates `out_ptr`. On a bad pointer (null,
out-of-user-range, or unmapped page) it returns -1 but the record has already
been consumed — the death notification for a crashed server is **permanently
lost**; a retry returns 0. The ABI contract ("1 = copied, 0 = none pending,
-1 = bad pointer") implies a failed recv is non-consuming. In the exact
fault-isolation scenario this feature exists for (a supervisor that has been
partially corrupted), a bad pointer silently disables crash notification for
its servers.

**Fix (included in rejected_patch.diff):** validate the destination pointer
(non-null + `CheckedPtr::valid()` range check) **before** calling `recv()`, for
both the user and kernel paths.

### S3 findings (non-blocking, recommended hardening)

- **S3-1 — Spec invariant #4 not implemented; poke-safety rationale overstated.**
  Spec death-notify.md says "id + generation revalidated at poke time". The
  code collects only `supervisor_id` into `pokes[]` (death_notify.cpp:311-326)
  and at poke time re-checks only id + liveness (334-337) — **generation is
  never re-checked**. Ids are monotonically allocated with no reuse
  (scheduler.cpp:1101-1105), so the wrong-target poke (id reuse → spurious
  `DEATH_WAKE_PULSE` to an innocent task) is currently unreachable, but the
  documented invariant is false and `Notify::notify()`'s guards protect the
  *waiter*, not a freed `this`. Recommend: capture `supervisor_gen` in `pokes[]`
  and compare at poke time; correct the spec.
- **S3-2 — Doc claims "s_lock_ is never acquired in IRQ context / under
  scheduler_lock_" are false.** `on_tick` holds `scheduler_lock_` (try_lock,
  scheduler.cpp:1244-1246) and calls `flush_zombies` (1731, NOT gated on
  `is_test_active()`) → `cleanup()` → `on_task_death()` → `s_lock_`. No lock
  cycle exists (no path acquires `scheduler_lock_` while holding `s_lock_`;
  `watch()`'s `find_task` under `s_lock_` is lock-free), so this is not a
  deadlock by ordering — but a task preempted while holding `s_lock_` with the
  zombie-watchdog tripped would make the ISR spin on `s_lock_` (theoretical
  hang). This is the same pre-existing risk class as cleanup()'s other object
  locks, so not a new S1/S2; correct the doc and consider an IrqGuard or
  try-lock for the ISR-reachable `s_lock_` acquisition.
- **S3-3 — Redundant `drain_task(*this)` in task.cpp:1789.** `on_task_death()`
  already ends with `drain_task(dead)`; the explicit call is idempotent (the
  `state != FREE` guard prevents a double tracker-decrement) but redundant.
- **S3-4 — Dead code.** `DeathNotify::slot()` (hpp:121, cpp:38) is declared and
  defined but never called.
- **S3-5 — Style regression.** syscall.hpp:320 `sys_frame_create` declaration
  lost its 4-space indent (continuation aligned to 38 spaces). check-style
  reports Errors 0 (the validator does not check this), but the line is
  inconsistent with the surrounding declarations.
- **S3-6 — Test coverage gaps / fragility.** (a) No test exercises the
  **user-mode** pointer path of `sys_death_recv` (`syscall_is_user_task()` is
  always false in the harness); `death_recv_nonblocking_none` calls
  `DeathNotify::recv` directly rather than through the syscall. (b)
  `death_watch_supervisor_death_drains` creates its supervisor at priority 53
  (above the harness), contradicting the test file's own priority-starvation
  note; it passes empirically but is fragile to scheduler changes.

### Checks that PASS (verified against source)

1. **Exactly-once latch** — `on_task_death` fires only from `cleanup()`; the
   single-funnel property holds (terminate → release_zombie removes from
   id_table_ once; drain_zombie_list/cleanup_step/flush_zombies each pop a
   zombie once; `destroy()` guards on REAPED; magic=0 skips a re-reap). The
   ACTIVE→PENDING transition is itself idempotent against a hypothetical double
   call. `drain_task` cannot reach the same slot twice (state!=FREE guard);
   `death_watch_exactly_once` (double `drain_zombie_list`) verifies one record.
   Hook placement (task.cpp:1788-1789) is after `daemon::notify_death` (1780),
   before fd/stack/page-table/`~Notify` (1791-1883) teardown — the drain runs
   before the supervisor's Notify is destroyed.
2. **Slot lifecycle exactly-once release** — all FREE transitions (unwatch,
   recv, drain) are guarded by `state != FREE`; at most one non-FREE→FREE
   transition per slot; `on_task_death` (ACTIVE→PENDING) does not touch the
   tracker; snapshot_reset zeroes. Tracker add/remove is balanced on every code
   path (but the balance is never checked — S2-1).
3. **Lock ordering** — `s_lock_` is a leaf; never held across a poke,
   `Notify::notify()`, or any `scheduler_lock_` acquisition (`find_task` under
   `s_lock_` is a lock-free id-table probe). `on_task_death` latches under
   `s_lock_`, pokes outside, and calls `drain_task` only after the latch scope
   ends (no re-entrancy). The single-direction `scheduler_lock_ → s_lock_`
   ordering in the watchdog path is safe (no cycle) — S3-2 documents the
   doc/code mismatch.
4. **Poke safety** — id-table removal at terminate + 0xDD magic poison +
   Notify's own waiter guards. No S1 (see above); spec rationale overstated
   (S3-1).
5. **watch() authority + race** — authority gate correct (`actor == watched ||
   actor == sup`, self-watch denied); liveness re-validated under `s_lock_` at
   install; the register/death race is rejected (-1) or recovered by
   unwatch/drain and is documented in the spec.
6. **recv() pointer path** — CheckedPtr SMAP handling (stac/clac +
   `g_user_access_recover_ip`) is correct; null-check present; never blocks.
   Defect: consume-before-validate (S2-2).
7. **Syscall table integrity** — DEATH_WATCH=66 / DEATH_RECV=67 /
   DEATH_UNWATCH=68, MAX_SYSCALL=69, table has 69 entries (indices 0-68,
   syscall.hpp:334-404), `handle()`/`handle_fast()` bounds-check `number >= 69`
   (syscall.cpp:126,162). The 3 new syscalls are NOT in `k_syscall_fast[]`
   (syscall.hpp:173-183).
8. **Fan-in + registry full** — `pokes[kMaxWatches]` is bounded (≤1 poke per
   slot); `watch()` fails closed at exhaustion; `recv()` consumes exactly one
   slot per call.
9. **Part A (External Pager) NOT implemented** — no pager / #PF-delegation code
   anywhere in the patch; correctly deferred.
10. **Test honesty** — the 9 tests are non-vacuous: real syscalls via
    `Syscall::handle`, real `terminate`+`drain_zombie_list` death path, task ids
    captured before termination (avoids reading freed TCBs). ResourceTracker
    zero-delta claim is **false as implemented** (S2-1).

## PATCH

`audits/rejected_patch.diff` — git-apply-able (verified: `git apply --check`
clean). Three files, addressing both S2 findings:

1. `src/kernel/test/resource_tracker.cpp` — add `death_watches` to `any_leak()`
   so the counter is actually verified.
2. `src/kernel/syscall/syscall_handlers_death.cpp` — validate the output
   pointer before `recv()` consumes the record (both user and kernel paths).
3. `src/kernel/test/test_cap_death.cpp` — drain the leftover PENDING records in
   `death_watch_registry_full_fails_closed` teardown so the death_watches
   counter is genuinely zero-delta once (1) lands.

Apply verbatim, re-build (`make build`), re-run `cap_death` + the debug gate
(`make execute-test x86_64 debug all`), then re-audit.

## DECISION

**DECISION: REJECTED**