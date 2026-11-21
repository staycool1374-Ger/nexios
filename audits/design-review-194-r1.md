# DESIGN REVIEW R1 — plan-194.md (userspace timer service)

Base: `79203e597` verified (`git log --oneline -1`). Scope: design audit of
`/Users/arnold/jarvis/audits/plan-194.md` against the tree. No production
code reviewed; no patch attached. Commit: NOTHING (review artifact only).

Method: every load-bearing plan claim re-checked in-tree (file:line below).
Scaffold (#241) confirmed present: `CONFIG_TIMER_SRV` default 0
(`nexios_config.h:644-658,1517-1524`), seams at
`syscall_handlers_posix_time.cpp:175/211/314/446`, `TimerRpc` 48 B +
epoch/tombstone/`send_sync_bounded` (`time/timer_srv.{hpp,cpp}`),
`userspace/timer-srv.c` skeleton (BIND/CLOCK_GETTIME/CANCEL_OWNERS live,
TIMER_* → -38), `drv-rtc` TaskDef (`taskdefs.cpp:363-369`) + `PdRole::RTC`
row (`boot_sdf.cpp:355-371`). #241 merge confirmed (`b28a13383`).
`ns_monotonic` rewind-proof confirmed (`x86_64/hal/timer.cpp:370-385`:
TSC/HPET + `monotonic_commit`; `set_ticks_for_test` touches `ticks_` only;
`test_isolate.cpp:1526` restores ticks only) — the #121 correction is sound.
`sys_receive` returns `msg.type` (`syscall_handlers_ipc.cpp:223`), so the
`rc != req.op` torn-payload idiom is valid. `SYS_GETTOD` reads
`arch::RTC::read_seconds()` (`syscall_handlers_misc.cpp:237`) — step 28
targets the right line. riscv mtime-1970 fallback confirmed
(`riscv64/rtc.cpp:126,141-149`); riscv `remaining()==0` confirmed
(`arch/hal/timer.hpp:262-267`).

## FINDINGS

- [S1] plan-194.md P3 step 14 — server main loop passes timeout 0 (= block
  forever) when a deadline is already due.
  WHY: `recv_wait_arm` with `timeout_ticks==0` arms nothing and returns true
  (`ipc/ipc.cpp:1826-1828`; `syscall_handlers_ipc.cpp:63-66`: "0 = block
  forever"), and step 14 computes `timeout_ticks = ceil((earliest-now)/tick)`
  clamped to `[0, kMaxWaitTicks]`, which is 0 exactly when `earliest <= now`.
  `fire_due` runs only after `recv` returns, so a due sleep/timer with no
  further message traffic blocks the server (and the nanosleep client)
  forever. The scaffold has the same trap (`userspace/timer-srv.c:91-99`:
  due deadline leaves `timeout==0`). Fix: fire-due-before-block, never pass
  0 for a due deadline (clamp minimum 1 tick / poll-then-fire), specify
  `kMaxWaitTicks`, and use overflow-safe ceil division (`(a/b)+((a%b)!=0)`;
  cf. the kernel's own decomposition at `ipc/ipc.cpp:1832-1841`).

- [S2] INV-T3 ("zero scheduler surface", no `scheduler_tick.cpp` diff) is
  unimplementable as specified alongside STAYS/MOVES.
  WHY: `scheduler_tick.cpp:576` calls `PosixTime::reapply_wakes` unconditionally
  and `:550-573` scans every task for `sleep_armed`/`sleep_expired` each tick;
  `reapply_wakes` takes `IrqSpinLockGuard(lock_)` per tick
  (`time/posix_time.cpp:533`). The plan neither guards the two tick blocks
  (a `scheduler_tick.cpp` diff, breaking the fence as literally stated) nor
  specifies the no-`scheduler_tick`-diff mechanism (stub `reapply_wakes` to a
  no-op under ON, document/quantify the dead sleep scan). One sentence must
  choose.

- [S2] Syscall 33 (ALARM) is unaccounted: normative spec says MOVE, plan says
  neither STAYS nor MOVES.
  WHY: `docs/specs/microkernel-transition.md` §8.4: "Syscalls: moves 33, 34,
  80–83". Plan covers 80–83 + GETTOD(34) but never mentions `sys_alarm`
  (`syscall_handlers_sync.cpp:90-108`), `alarm_ticks`/`alarm_armed`, or the
  tick countdown + `sleep_armed`-only wake (`task/scheduler_tick.cpp:841-855`).
  Decide: STAYS (per-task tick-counter signal delivery is kernel-internal;
  only the sleeper-wake at `:851-853` goes dead) or MOVES with design.

- [S2] Signal EINTR for `nsleep_wait`-ers is lost under ON (regression vs OFF
  leg, no decision recorded).
  WHY: delivery wakes only `sleep_armed` BLOCKED tasks
  (`proc/proc_svc.cpp:620-621`; alarm tick `scheduler_tick.cpp:851-853`).
  Under ON the sleeper parks in `IPC::recv` with `sleep_armed==false`, and
  step 24 specifies no pending-signal probe (the OFF leg probes at
  `syscall_handlers_posix_time.cpp:250-277`). Scaffold documents "no
  EINTR path yet"; the plan must either carry that limitation explicitly
  (with test) or specify probe + delivery wake for recv-parked sleepers.

- [S2] INV-T5 drop-side guarantee is unenforceable as specified for
  `timer_delete`/timerfd (no drop site with live state).
  WHY: proxy validation checks sender/epoch/owner only, not
  timer_id/slot_gen (`time/timer_srv.cpp:106-111`), while INV-T2 bans
  kernel-side slot tables — so an already-sent expiry vs a racing
  `timer_delete` (same owner, slot reused, no waiter) has no kernel state to
  check against. `timerfd_read_entry` (`syscall_handlers_posix_time.cpp:508+`)
  has no ON leg at all today, and step 27's "slot freed locally" contradicts
  INV-T2 unless the vnode tag carries the server-issued (slot,gen) opaquely
  with deliver-side token compare. Specify the tag contents + check site.

- [S2] `DRV_OP_RTC_EPOCH=2` / `DRV_OP_RTC_READ=3` collide with the shared
  `DrvOp` wire enum.
  WHY: `driver/driver_pds.hpp:62-73` (mirrored `userspace/driver_pd.h:13-17`)
  already defines `WRITE=2`, `POLL=3` on the same one-byte op field; reusing
  them for RTC-epoch/read creates dispatch ambiguity. Use fresh numbers
  (≥10) in both headers.

- [S2] False-close sequencing: retirement step 36 conditions OFF-code deletion
  on "after QE's rewrite lands", but close criteria are not pinned to it.
  WHY: #194 may only close when POSIX is provably out of kernel (grep-clean
  ON leg, increment 2 done) AND `srv_timer` 3/3 + timing classes green on the
  release matrix. State that in the plan or the flip can close with the OFF
  leg retained and the QE port "in flight".

- [S3] Watchdog precedent citation is imprecise.
  WHY: `userspace/watchdogd.c:207` is a raw `__syscall5(2,...,10)`, not the
  `ipc_recv_timeout` wrapper (only `timer-srv.c`/`bind-verify.c` use the
  wrapper). Substance holds (bounded-receive precedent + `r != msg.type`
  idiom at `watchdogd.c:210`). Fix the citation.

- [S3] Gotcha-number citations do not match the vault.
  WHY: plan cites "gotcha #21" (mtime-1970) and "#11" (Liu-Leyland); the
  vault's #21 is exec-ns metering (`prompts/LEARNINGS.md:1200`) and the mtime
  fallback is documented under the #355 lineage (`LEARNINGS.md:1475`, BRIEFING
  gotcha #22). Facts verified in-tree; fix the numbers.

- [S3] #241 S3b tombstone drift direction unspecified (and §10 D6 unmet).
  WHY: spec says "drops oldest" (`docs/specs/timer-srv.md:21`), code drops
  newest past 16 with no counter (`time/timer_srv.cpp:74-82`), and normative
  §8.4/§10 D6 requires overflow counted and reported. P2/P6 claims the fix
  without stating direction — specify drop-newest + overflow counter +
  spec update.

- [S3] Wake-source deviation from normative §8.4 unstated.
  WHY: spec says "timer-srv's own wake source is a tick Notify from the
  kernel" (`microkernel-transition.md` §8.4); plan uses `ipc_recv_timeout`
  and rewrites §6/§14 but not §8.4. Reconcile (one line + spec edit).

- [S3] Retrieval row 6 cites `audits/report-20261029T002722Z-241.md`, which is
  absent from `audits/` (only `plan-194.md` + one #47 report present).
  WHY: check-7 evidence must be locatable; record the vault path or attach
  the artifact. `daemon_mgr.cpp:276-278` (budget ternary, outside `#if`)
  and `:298-303` (restart, inside `#if`) verified live, so the S3a fix
  target is real — only the citation/line drift needs repair.

- [S3] Tombstone drain is piggyback-only (`CANCEL_OWNERS` on the next proxy
  RPC): dead-owner server slots with no subsequent traffic linger until table
  pressure (`-EAGAIN` fail-closed).
  WHY: availability degrades silently under churn-with-idle; accept with a
  stated bound or add epoch/periodic drain.

- [S3] `test_isolate` `snapshot_reset` hookup (INV-T6) is assigned to neither
  dev nor QE.
  WHY: `test_isolate.cpp` is QE-owned test code on `testbed`; the epoch-bump
  call site must be explicitly QE-owned or the purge has no implementer.

## PATCH

No patch: design review only, nothing to `git apply`.

DECISION: REJECTED
