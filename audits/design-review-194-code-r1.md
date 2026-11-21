# AUDIT REPORT 20261121T152053Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/microkernel-transition.md, docs/specs/timer-srv.md, src/kernel/daemon/daemon_mgr.cpp, src/kernel/driver/driver_pds.cpp, src/kernel/driver/driver_pds.hpp, src/kernel/kernel.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall_handlers_misc.cpp, src/kernel/syscall/syscall_handlers_posix_time.cpp, src/kernel/task/boot_sdf.cpp, src/kernel/task/boot_sdf.hpp, src/kernel/task/taskdefs.cpp, src/kernel/task/tcb_control.cpp, src/kernel/time/posix_time.cpp, src/kernel/time/timer_srv.cpp, src/kernel/time/timer_srv.hpp, userspace/driver_pd.h, userspace/rtc-pd.c, userspace/timer-srv.c

Scope: uncommitted production diff on main (base 79203e597, byte-identical to
worktree diff, 19 files, no test files). test-history.txt + graphify-out/
excluded (evidence/tool artifacts, not patch content). Method: full patch read
(3474 lines) + targeted in-tree verification of every load-bearing claim
(handler routing, IPC wake discipline vs send_sync_err, SDF/role tables,
config defaults, gate evidence via gh #605).

## FINDINGS
- [S2] src/kernel/task/taskdefs.cpp:127-129 (pd_restart_rtc) + src/kernel/time/timer_srv.cpp:520-539 (refresh_anchor) — plan step 31 restart-resync unimplemented; code comment asserts a mechanism that does not exist.
  WHY: pd_restart_rtc does caps-only rebind (sdf_rebind sends no BIND) + anchor_invalidate, but no caller ever re-sends the rendezvous BIND (pd_bind_rtc callers: kernel.cpp boot + ensure_bound/timer-epoch only; init-loop DAEMON_READY only logs) or commands a re-push (pd_rtc_push_epoch likewise uncalled on this path), while rtc-pd gates its feed on g_server_pid (set only from BIND arg1) — so the respawned PD never feeds, and the invalidated cell is re-latched STALE from the server (refresh polls the server, not the PD) with clear flags: silent frozen-offset wall clock until an unrelated timer-srv epoch bump. The comment "an rtc-pd restart re-feeds the server epoch" is factually false, and QE test 3's restart-resync half cannot pass as written.
- [S3] src/kernel/time/timer_srv.cpp:471-510 (anchor cell) — multi-writer transient: 2x u64 + flags published data-RELAXED + valid-RELEASE is tear-free (8 B single-copy-atomic on all three arches, verified layout) but concurrent publishers (any task via refresh/boot-fallback) can invert the monotone guard or mix anchor/mono_at pairs (bounded poll-interval skew, self-corrects next read).
  WHY: No single-writer discipline or seqlock; benign for a wall clock but the "only advances" claim holds only under cooperation — harden (single-writer or sequence lock) if the cell ever feeds control decisions.
- [S3] src/kernel/time/timer_srv.cpp:606-628 (timerfd_deliver doc) — docstring promises "-EBADF stale" but the implementation returns only 0/-EAGAIN (a stale tag parks until server death, which degrades to -EAGAIN; fail-closed behavior is correct, the doc overstates it).
  WHY: Comment/implementation mismatch; unreachable via in-tree paths (close re-tags/disarm before free), so doc-only fix.
- [S3] src/kernel/syscall/syscall_handlers_posix_time.cpp:609-650 (ON timerfd_create) — errno inconsistency: vnode-alloc failure returns GENERIC_FAIL while fd-exhaustion returns -EAGAIN.
  WHY: Both fail-closed, but the same syscall maps equivalent resource failures to two errnos — align to -EAGAIN.
- [S3] test-history.txt (evidence hygiene, not patch content) — the 16:09:54 release selftest 87 row carries no STATUS: STALL-RECOVERED annotation for the claimed stall.
  WHY: AGENTS.md requires STATUS: on abnormal termination; the run itself is clean (87/0, measured TIME), so annotation-only.

## VERIFICATION (12-item checklist disposition)
1. P0 loop: PASS — fire-due-first on both wakes, min-1 clamp, kMaxWaitTicks=4000 next to define, overflow-safe ceil (mirrors ipc.cpp:1832-1841), no live 0-timeout (0 only for no-deadline idle), scaffold trap removed, calibration bounded (64 iters, div-by-zero guarded).
2. Wire v2: PASS — ops 8-12 in both headers, flags bits both sides, reply type 0xF0000002 == userspace MSG_TYPE_REPLY, version 2 both sides with BIND -EPROTO gate, 48 B static_assert holding (1+1+2+4+4+4+8+8+8+2+2+4).
3. Anchor cell: PASS with S3 (see above) — lock-free publish, monotone guard, boot fallback, saturating add, tear-free.
4. nsleep: PASS — one-way send (blocking-on-full == established send_sync idiom, server-drain wake) + wait_for_fire mirrors validated send_sync_err discipline line-for-line (BLOCKED spin, bounded drain, was_blocked-gated gen check, IrqGuard publish + Dekker probe, dequeue-if-queued, remaining_ticks + #513 sti); sender+epoch+owner+token filter; death->-EINTR + saturating remainder; grep-clean of TimerWheel/PosixTime arms (comment mention only).
5. timerfd_deliver: PASS — tag-vs-token compare-then-wake, disarm gen-bump + kernel re-tag, death->-EAGAIN, read_entry ON leg present, deliver-and-clear per period.
6. Tombstones: PASS — drop-newest past 16 + g_tombstone_drops_total + spec rewritten to drop-newest; kernel one-way CANCEL_OWNERS drain + explicit sync path; server fire_due sweeps pending purges first.
7. posix_time.cpp: PASS — single hunk, reapply_wakes stub only; no scheduler_tick/queue/debug/RQM file in patch (fence holds).
8. ALARM-33/EINTR: PASS — syscall 33 + alarm tick untouched (no sync-handler/scheduler files in patch); STAYS recorded in both spec docs; EINTR limitation in code comment (timer_srv.cpp header + handlers) + spec + plan-pinned QE test name (QE-owned, correctly absent from this diff).
9. DRV_OP: PASS — 10/11 fresh in both headers; WRITE=2/POLL=3/BIND_FRAME=9 confirmed, no collision.
10. Knobs/roles: PASS — CONFIG_TIMER_SRV default 0 retained, 0/1 + range #errors kept, increment-1 only; TIMER_SRV=14, kPdRoleCount=15 + both static_asserts + server_params_valid bound + zero-caps/policy-only/owns-device rows; tcb_control property-based (no exclusion); daemon_mgr all four sites fixed (both functions x budget/restart), only comments mention timer-srv.
11. Style + gates: PASS — Errors 0 independently re-run (src 578 files, userspace 73 files); evidence rows present with measured TIME for both legs + repeats consistent with off-arch shape; core_syscall 88/1 + capability 198/2 identical OFF/ON and independently pinned to OPEN #605 (#564 Stage C drift, verified via gh, "not caused by #194"); posix ON 10/14 = 4 stub-shape QE pins per plan; release selftest 87 (minus S3 annotation); ipc 105/106 zero-fail delta is an observation, not a failure.
12. r2 S3s + A1-A6: PASS — ensure_running both sites fixed, §8.4 mechanism sentence present; all six deviations present and sound (stale-poll anchor, per-period deliver-clear, 32-entry sleep table, rtc re-push op path, fd-tag re-tag, fire-status -EAGAIN).
Recorded plan hole (S3, not patch-rejectable): feed-LOSS (kill-half) DEGRADED detection has no specified or implemented detector — server has no feed-age tracking, kernel has no feed death-watch, and the grace would need feed-period semantics (no periodic re-push exists), so it cannot be fixed minimally here; prescribe periodic re-push or death-watch + grace, pinned by QE test 3 before close (close criteria already require test 3 green).

## PATCH
`audits/rejected_patch.diff` was written (git-apply-verified, 2 hunks): refresh_anchor performs the lazy task-context re-rendezvous (pd_bind_rtc + pd_rtc_push_epoch) before polling the server, and the pd_restart_rtc comment is corrected to describe the deferred mechanism with the tick-context rationale. This closes S2-A; S3s ride along as advisories.

DECISION: REJECTED
