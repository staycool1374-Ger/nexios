# F20 libc syscall audit (#572) — PAUSE=36 retirement inventory + 34/80–83 MOVE preconditions

Provenance: planner retrieval (graphify 455 nodes; vault → in-repo T-1,
F20 anchor) + plan-572.md step 1; auditor plan-APPROVED
(report-20261127-572-plan.md). All commands run on main @ e51347e0d.

## PAUSE=36 — verdict: RETIRE unblocked (0 callers, 0 definitions)

Dependency set (exactly 6 symbols, 4 files):
`src/libc/syscall.h:69` (`#define SYS_PAUSE 36`), `:238-240` (`sys_pause()`
inline), `src/libc/gen/nexios_syscalls.c:258-262` + `.h:322-330`
(generated `nexios_sys_pause`), `src/libc/unistd.h:61` +
`src/libc/signal.h:58` (`int pause(void);` declarations).

- `rg 'SYS_PAUSE|sys_pause|nexios_sys_pause' src/ userspace/ tools/ docs/`:
  the 6 symbols above only. **0 call sites** (`test_spinlock.cpp:260`
  is a comment about `pause()`-iteration counting, unrelated symbol).
- `rg '^\\s*(int|void)\\s+pause\\s*\\(void\\)\\s*\\{' src/ userspace/`:
  **empty** — no definition anywhere.
- `rg '\\b36\\b' src/kernel/syscall/ src/libc/syscall.h docs/specs/`:
  define + enum + T-1 row only (plus one unrelated `:36` line reference).
- Tracked binaries (`git ls-files '*.elf'`): `hello.c.elf`,
  `libc_verify.c.elf` — both built from audited in-tree sources; neither
  consumes 36 (no reference in their sources).
- Semantic finding: the kernel handler is a single `arch::hlt()`
  (`syscall_handlers_misc.cpp:275-279`) — NOT POSIX `pause()` (no
  block-until-signal, no EINTR). Retirement loses no behavioural contract;
  libc's declared `pause()` was never implementable on it.

## 34/80–83 MOVE — verdict: BLOCKED, parks as enabling evidence for #194

Live surface: kernel handlers (`sys_gettod` misc:215;
`sys_clock_gettime`/:180, `sys_nanosleep`/:219, `sys_timer_create`/:327,
`sys_timerfd_create`/:463 in posix_time.cpp); libc `time.c`
(`time`:15, `gettimeofday`:28, `clock_gettime`:41, `sleep`:49,
`nanosleep`:67); in-tree userspace clients incl. `timer-srv.c:151`,
`timer-srv-bench.c`, `rtc-pd.c:228`, `prime.c`, `driver-pd-parity.c`,
`stub-roundtrip.c`, `xmodem.c`, `sh.c`.

Blocking preconditions (posted to #194):
- **P1 (hard, self-RPC):** `timer-srv.c:147-161 tmr_now_ns()` reads
  `clock_gettime(CLOCK_MONOTONIC)` = syscall 80. Once 80 is a timer-srv
  RPC, the PD is its own client. Must re-base on `SYS_GET_TICKS` (5) or a
  kernel-private MONOTONIC op — a #194 design decision, not an F20 edit.
- **P2 (same-release coupling):** libc must switch to a timer-srv client
  library in the SAME increment that deletes the `CONFIG_TIMER_SRV=0`
  leg (`syscall_handlers_posix_time.cpp` seams). A split release breaks
  every libc time caller on the flip.
- **P3 (34 coupling):** `time.c:17-35` GETTOD fallback path retires
  together with slot 34, not before.
- **P4 (QE handoff):** numbers pinned in QE-owned tests (`test_syscall.cpp`
  gettod legs, `posix_time`/`srv_timer` classes) — retirement needs a
  QE test-tree increment in the same cycle.
