# DESIGN REVIEW R2 — plan-194.md rev 2 (userspace timer service)

Base: `79203e597` verified (`git log --oneline -1`). Scope: design audit of
`/Users/arnold/jarvis/audits/plan-194.md` rev 2 against the tree + against
`audits/design-review-194-r1.md` (REJECTED: 1 S1 + 6 S2 + 8 S3). No production
code reviewed; no patch attached. Commit: NOTHING (review artifact only).

Method: every r1 finding re-checked against rev-2 text AND re-verified live
in-tree (file:line below). Rev-2 ferry note present (plan-194.md:10-18 blockquote).

## R1 FINDING DISPOSITION (15/15 verified fixed — each fix is real, not asserted)

- [S1 fixed] Step 14 (plan:114) is now fire-due-FIRST on every wake, computes
  `timeout_ticks = ceil((earliest-now)/g_ns_per_tick)` with overflow-safe ceil
  (`(a/b)+((a%b)!=0)`, matches the kernel decomposition at
  `ipc/ipc.cpp:1832-1841`), clamped to `[1, kMaxWaitTicks]` with
  `kMaxWaitTicks = 4000` documented next to the define. The 0 semantic is
  cited live (`ipc/ipc.cpp:1826-1828`: `timeout_ticks == 0` returns true with
  no arm = block-forever) and the scaffold trap is named and removed
  (`userspace/timer-srv.c:91-99`: due deadline leaves `timeout==0` — verified
  live, lines 91-97; `tmr_fire_due` only runs AFTER `recv` at :101). No
  remaining 0-timeout path for a live deadline: the clamp range floor is 1 and
  applies exactly to the live-deadline case; idle (no deadline) block-forever
  is message-woken, which is the correct server idle. Div-by-zero guarded by
  step 13 fallback (1 ms/tick + DEGRADED).
- [S2 fixed — INV-T3] One sentence chooses (plan:70): `PosixTime::reapply_wakes`
  stubbed to no-op under `CONFIG_TIMER_SRV=1`, stub lives in
  `time/posix_time.cpp` (verified non-fenced TU; `reapply_wakes` at
  `posix_time.cpp:532` with `IrqSpinLockGuard` at :533). The dead scan is
  documented/quantified: `scheduler_tick.cpp:550-573` stays, provably finds
  nothing under ON (park in `IPC::recv`, never `sleep_armed`), bounded reads
  only, measured idle cost posted with gates. Fence literally holds: step 2
  (plan:96) requires `git diff --stat` to show zero
  `scheduler_tick/queue/debug` + `ready_queue_manager` edits every iteration.
  Live anchor confirmed: `scheduler_tick.cpp:576` calls `reapply_wakes`
  unconditionally — the stub (not the caller) is what changes, so no fenced
  diff is needed. Coherent.
- [S2 fixed — ALARM-33] STAYS explicit (plan:65): `sys_alarm`
  (`syscall_handlers_sync.cpp:90-108`), `alarm_ticks`/`alarm_armed`, tick
  countdown + `sleep_armed`-only wake (`scheduler_tick.cpp:842-855`, verified
  live) named; sleeper-wake leg at `:851-853` declared dead under ON with the
  reason (no sleeper sets `sleep_armed` there anymore). MOVES list (plan:64)
  no longer claims 33. Decided, not unaccounted.
- [S2 fixed — EINTR] Carried limitation explicit (plan:126): no pending-signal
  probe for recv-parked sleepers under ON, OFF-leg probe cited
  (`syscall_handlers_posix_time.cpp:250-277`), delivery-wake mechanism cited
  (`proc_svc.cpp:620-621`, alarm tick `:851-853`), consequence stated
  (signal deferred until sleep completes or server dies; server-death EINTR
  still works). Pinned by named QE test
  `srv_timer::nanosleep_signal_deferred` with signal-then-reply-first
  semantics. Oversight converted to a specified, tested limitation.
- [S2 fixed — INV-T5] Tag contents + check site specified (plan:74): vnode
  carries server-issued `(slot, slot_gen)` opaquely at create time (never
  interpreted by VFS, never a kernel deadline table — INV-T2 reconciled in
  the same paragraph); `timerfd_read_entry` gains an ON leg comparing event
  token vs vnode tag, drop-before-wake (stale reuse + post-delete in-flight
  die there). Step 27 (plan:129) consistent: names
  `syscall_handlers_posix_time.cpp:508+` (verified live: `timerfd_read_entry`
  at :508, waiter calls at :550-622) and performs the tag-vs-token compare;
  OFF leg untouched. Proxy validation gap (sender/epoch/owner only,
  `timer_srv.cpp:106-111`) is now covered by the stated drop site.
- [S2 fixed — DRV_OP] 10/11 in BOTH headers (plan:49-50). Collision verified
  live: `driver_pds.hpp:62-73` (`WRITE=2`, `POLL=3`) and
  `userspace/driver_pd.h:13-17` (`WRITE=2`, `POLL=3`, used through 9 + 240),
  so 10/11 are fresh on the shared one-byte op field in both. Fix real.
- [S2 fixed — close criteria] Pinned to increment 2 (plan:142): #194 closes
  ONLY on (a) grep-clean ON leg + every OFF body deleted AND (b) `srv_timer`
  3/3 + `timing_core`/`hrt_monotonic` green on the full 2×3 release matrix.
  Matches the r1 demand (POSIX provably out + release-matrix green).
- [S3 fixed — watchdog cite] Corrected (plan:70): raw `__syscall5` + `r !=
  msg.type` check at `watchdogd.c:207-210` (verified live), wrapper
  disclaimed (only `timer-srv.c`/`bind-verify.c` use it). Substance intact.
- [S3 fixed — gotcha numbers] (plan:84/86): no LL number cited (vault #11
  correctly identified as exec-ns metering, not LL); mtime-1970 under
  BRIEFING #22 + vault #355 lineage + `LEARNINGS.md:1475`; `remaining()`
  hard-0 retained as gotcha #20 with "undisputed" qualifier. Grep of the plan
  shows no remaining `#11`-for-LL or `#21`-for-mtime. Verified live where it
  matters (`driver_pds.hpp` collision above; riscv/rtc + hal/timer already
  verified in r1 and unchanged).
- [S3 fixed — tombstone] Direction + counter + spec aligned three places:
  step 41 (drop-newest past 16, code-is-truth `timer_srv.cpp:74-82` —
  verified live: `count >= 16` early-return at :76 = newest dropped, no
  counter today), step 19 (plan:119: bound + `tombstone_drops_total_`), S3
  risk line (plan:191: spec rewritten to drop-newest + counter). P2/P6/S3
  agree; live spec line `timer-srv.md:21` ("drops oldest") named as the
  stale text updated in the same commit. §8.4/§10 D6 counting met by the
  specified monotonic counter.
- [S3 fixed — §8.4 reconcile] One line + spec edit present (plan:54): the
  Notify IS the wheel-expiry message wake (`ipc_recv_timeout` returns on it;
  no per-tick broadcast added), no §8.4 semantic change, mechanism sentence
  only. Deviation now stated. (See new S3-2 below for a commit-step nit.)
- [S3 fixed — retrieval cite] Repaired (plan:29): absent
  `report-20261029T002722Z-241.md` recorded as absent-from-`audits/` (no
  longer a dangling check-7 pointer), vault-index citation qualified, live
  re-verification cited (`daemon_mgr.cpp:271-278`; tombstone spec/code
  mismatch). Trail is now honest.
- [S3 fixed — piggyback bound] Stated bound (plan:119): every `fire_due`
  pass sweeps dead-owner tombstones first (server-epoch check + release), so
  linger ≤ one wake interval; `CANCEL_OWNERS` stays the explicit path;
  `-EAGAIN` fail-closed at table-full; `tombstone_drops_total_` counts.
  R1 asked "stated bound or periodic drain" — a bound is stated. Accepted.
- [S3 fixed — snapshot_reset owner] Explicitly QE-owned (plan:76):
  `test_isolate.cpp` call-site edit is QE's (test file on `testbed`, dev
  must not touch); dev lane ships hook unconnected-but-specified. No
  implementer gap.
- [Regression — r1 verified-sound items intact] All present in rev 2:
  scaffold lines (`nexios_config.h:644-658/1517-1524`, handler seams
  `:175/211/314/446`, timer_srv 48 B + epoch/tombstone/`send_sync_bounded`,
  `timer-srv.c` BIND/CLOCK_GETTIME/CANCEL_OWNERS live / TIMER_* → -ENOSYS,
  `drv-rtc` TaskDef + `PdRole::RTC` row); 48 B `static_assert` (plan:40/100);
  `kPdRoleCount` truncation hazard (retrieval row 5 + S2 risk, plan:186);
  `daemon_mgr` ternary target (plan:48/110); `#121`/`set_ticks_for_test`
  correction (plan:34, INV-T4 plan:72); `sys_receive` torn-payload idiom
  (plan:114/119, `syscall_handlers_ipc.cpp:223` already verified r1);
  `SYS_GETTOD` line (plan:44, `:237` already verified r1); riscv fallbacks
  (plan:86). Nothing sound was dropped in the rewrite.

## NEW FINDINGS (2 × S3, implementation notes — neither blocks the design)

- [S3-1] Step 10 (plan:110) enumerates only the first `daemon_mgr.cpp`
  pair (`:276-278` budget ternary, `:298-303` restart `#if`) but
  `ensure_running()` duplicates both sites live at `:398-400` (timer-srv
  budget ternary) and `:417-422` (restart `#if`). Verified by direct read
  (:265-424). The step's own acceptance criterion — grep `timer-srv` → 0
  hits outside `taskdefs.cpp` — forces all four sites to be handled, so the
  design self-corrects at implementation; extend the deletion list to both
  functions (or rely on the grep gate). Hygiene/enumeration only, no safety
  impact (budget already comes from the TaskDef row).
- [S3-2] The §8.4 mechanism sentence is specified in step 54 (plan:54) but
  the docs commit step (plan:143, step 37) lists only "§6/§14 rows +
  `timer-srv.md` final state" — §8.4 is not named in the edit list. An
  implementer following step 37 literally could land the flip without the
  one-line §8.4 reconcile. Add "§8.4 mechanism sentence" to step 37's file
  list. Docs completeness only.

## PATCH

No patch: design review only, nothing to `git apply`.

DECISION: APPROVED (15/15 r1 findings verified fixed in-tree; 2 new S3
implementation notes S3-1/S3-2 carried, neither safety-blocking).
