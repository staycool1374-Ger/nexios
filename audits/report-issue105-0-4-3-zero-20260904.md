# AUDIT REPORT 20260904-071855

PATCH: audits/pending_patch.diff
FILES: docs/specs/death-notify.md, docs/specs/shm.md, src/kernel/ipc/death_notify.cpp, src/kernel/ipc/death_notify.hpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.hpp, src/kernel/syscall/syscall_handlers_death.cpp, src/kernel/task/task.cpp, src/kernel/test/resource_tracker.cpp, src/kernel/test/resource_tracker.hpp, src/kernel/test/test_cap_death.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/test/test_registry.cpp, src/lib/test.hpp

Scope: issue #105 Part B — Crash Supervisor / DeathNotify (audit iteration 2).
The patch was verified to apply cleanly to HEAD (fe3ecaa1, which includes the
#106 cap_shm work) and to reproduce the worktree byte-for-byte for all 15
files (`git apply --check` clean; `cmp` MATCH on every file).

## FINDINGS

### Remediation verification (iteration-1 findings)

- [VERIFIED] S2-1 — `any_leak()` now compares `current.death_watches >
  baseline.death_watches` (resource_tracker.cpp:107-108), so the counter is
  genuinely enforced. `death_watch_registry_full_fails_closed` now drains the
  leftover PENDING records at teardown (`while (recv == 1) {}`, test_cap_death.cpp
  ~353-357); the loop is bounded by `kMaxWatches` (each `recv` consumes exactly one
  PENDING slot or returns 0) so it cannot infinite-loop. Transaction audit for all
  9 tests: consume_roundtrip (+1/-1), crash_reason (+1/-1), multi_fan_in (+3/-3),
  after_death (0), supervisor_death_drains (+2/-2 via the dying supervisor's
  `cleanup() → on_task_death → drain_task`), registry_full (+kMax/-1/+1/-kMax),
  exactly_once (+1/-1), unwatch_before_death (0), recv_nonblocking_none (0) —
  every test leaves the counter balanced. Snapshot ordering confirmed: the leak
  check (test_isolate.cpp:717-726) runs BEFORE the registry `snapshot_reset()`
  (test_isolate.cpp:1364), so leftover records fail honestly.
- [VERIFIED] S2-2 — `sys_death_recv` validates the destination BEFORE `recv()`
  consumes a record, on both paths: user path `CheckedPtr.valid()` (non-null +
  user-range) at syscall_handlers_death.cpp:59-62, kernel path null check at
  :68-70; `got` is initialized (`= 0`, style gate). A null / kernel-range /
  out-of-range pointer is now strictly non-consuming. Residual hardening gap →
  S3-2 below.
- [VERIFIED] S3-1 — `on_task_death` collects `(supervisor_id, supervisor_gen)`
  pairs into a `Poke` array (death_notify.cpp:133-137, 151-154) and re-validates
  `sup->generation != pokes[i].gen` at poke time (:162-166). Spec invariant #4 is
  now true.
- [VERIFIED] S3-2 — death-notify.md lock-ordering section now states the real
  single-direction order (`scheduler_lock_ → s_lock_` via on_tick →
  flush_zombies → cleanup → on_task_death) and that no cycle is possible; the
  theoretical ISR-spin-on-`s_lock_` window is disclosed (:77-86). The poke path
  `notify() → set_task_ready` was re-checked: it does NOT re-acquire
  `scheduler_lock_` (IrqGuard + lock-free `find_task` + runq enqueue), so no
  re-entrancy under on_tick.
- [VERIFIED] S3-3 — the redundant `drain_task(*this)` was removed; `cleanup()`
  now calls only `DeathNotify::on_task_death(*this)` (task.cpp:1788), which ends
  with `drain_task(dead)` (death_notify.cpp:170).
- [VERIFIED] S3-4 — `slot()` declaration/definition removed (hpp/cpp).
- [NOT FIXED] S3-5 — syscall.hpp:320 `static uint64_t sys_frame_create(...)` is
  STILL at column 0 (no 4-space indent); the remediation claim that the indent
  was "restored" is false — the base (main) had the correct indent and the patch
  line `+static uint64_t sys_frame_create` is unindented. The 3 new declaration
  continuations are also unevenly aligned (37/37/39 spaces vs the surrounding
  35-37). check-style does not flag it, so this is cosmetic only → S3-1 below.

### New / remaining S3 findings (no S1, no S2)

- [S3] src/kernel/syscall/syscall.hpp:320 — style: `sys_frame_create` declaration
  lost its class-body indent (remediation claim for S3-5 is false); the three new
  `sys_death_*` continuation lines are not aligned with the surrounding
  declarations. Cosmetic; the file still compiles and check-style stays at Errors 0.
- [S3] src/kernel/syscall/syscall_handlers_death.cpp:59-67 — the S2-2 fix is
  partial: `CheckedPtr::valid()` is a RANGE check only (non-null + < USER_SPACE_LIMIT);
  it cannot detect an unmapped-but-in-range page. `copy_to()` can then #PF-recover
  to `recover_ct` AFTER `recv()` has already consumed the record (PENDING→FREE),
  so a partially-corrupted supervisor passing a valid-range pointer to an unmapped
  page still permanently loses the notification (returns -1; retry returns 0).
  Recommend a mapped-page probe (the `VMM::virt_to_phys_in_pml4(addr, read_cr3())`
  pattern used by `is_user_string`) before consuming, making the failure strictly
  non-consuming. Not S2: the intended common bad-pointer cases (null, kernel
  range, overflow) are handled, and a healthy supervisor always passes a mapped
  buffer.
- [S3] src/kernel/test/test_cap_death.cpp — `sys_death_recv` (the entire
  pointer-validation wrapper) has ZERO test coverage: all 9 tests call
  `ipc::DeathNotify::recv()` directly rather than through `Syscall::handle`; the
  user-mode SMAP path (`syscall_is_user_task()`) is never exercised (the harness
  is a kernel task). The S2-2 fix is therefore verified statically only.
  Suggest one kernel-path syscall test (valid + null out_ptr) and, if feasible,
  a user-task SMAP path test.
- [S3] src/kernel/test/test_cap_death.cpp:1046 — `death_watch_supervisor_death_drains`
  creates its supervisor at priority 53 (above the harness at 10), contradicting
  the file's own priority-starvation note; it passes empirically (the supervisor
  never runs) but is fragile to scheduler changes. (Unchanged from iteration-1
  S3-6(b); not claimed fixed.)

### Critical checks (re-run against the full patch)

1. Exactly-once death latch — PASS. `on_task_death` fires only from `cleanup()`
   (task.cpp:1788), the single funnel; terminate → release_zombie removes from
   id_table_ once, each zombie-list pop calls cleanup() once (magic guards), and
   the ACTIVE→PENDING transition is idempotent + generation-guarded.
2. Slot lifecycle exactly-once release — PASS. Every non-FREE→FREE transition
   (unwatch/recv/drain) is guarded by `state != FREE`; ACTIVE→PENDING changes no
   tracker count; add/remove balance verified on every path AND now enforced by
   `any_leak()`.
3. Lock ordering — PASS. `s_lock_` is a leaf; latch scope releases it before the
   pokes; `drain_task` re-acquires after (not re-entrant); no path acquires
   `scheduler_lock_` while holding `s_lock_` (`find_task` under `s_lock_` is a
   lock-free id-table probe); poke → `Notify::notify()` never takes
   `scheduler_lock_`; single direction `scheduler_lock_ → s_lock_` forms no cycle.
4. Poke safety incl. generation re-check — PASS. `(id, gen)` captured under the
   lock, re-validated at poke time; terminate removes from id_table_ (find_task →
   null); 0xDD magic poison + `Notify` TERMINATED/REAPED/waiter-gen guards cover
   the residual free-window (practically unreachable single-core). No S1/S2.
5. watch() authority + register/death race — PASS. Authority gate correct
   (`actor == watched || actor == sup`, self-watch denied); liveness re-validated
   under `s_lock_`; residual race (die-just-after-install) still latches a record
   and is documented.
6. recv() pointer path — PASS with S3-2 residual (above). validate-BEFORE-consume,
   SMAP via `copy_to` (stac/clac + recover), never blocks.
7. Syscall table integrity — PASS. DEATH_WATCH=66 / DEATH_RECV=67 /
   DEATH_UNWATCH=68 / MAX_SYSCALL=69; table has exactly 69 entries (indices
   0-68); `handle()`/`handle_fast()` bounds-check `>= 69`; the 3 syscalls are NOT
   in `k_syscall_fast[]` (pointer-free subset), so they keep the FULL-path canary
   + SMAP checks.
8. Multi-task fan-in + registry full — PASS. `pokes[kMaxWatches]` bounded (≤1
   poke per slot); `watch()` fails closed at exhaustion; `recv()` consumes one
   slot per call; `unwatch()` and `drain_task()` scan all slots.
9. Part A (External Pager) absent — PASS. No pager / #PF-delegation code anywhere
   in the patch; correctly deferred in death-notify.md.
10. Test honesty + ResourceTracker zero-delta — PASS. `any_leak()` enforces
    death_watches; registry-full drain loop is bounded and cannot infinite-loop;
    all 9 tests leave the counter balanced (transaction audit above). Weak
    spots: `seen` bitmask uses `dead_id % 64` (could false-pass only if two
    distinct ids collide mod 64, not the case for the sequential test ids) and
    `sys_death_recv` is never exercised (S3-3) — neither is an S1/S2.

## DECISION

DECISION: APPROVED