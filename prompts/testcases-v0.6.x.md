# Test Cases — v0.6.x (Phase 7: Safety Systems)

## Branch: testbed only

*Outline — test details to be expanded when implementation begins.*

### Watchdog Init — test_watchdog_init.cpp
- ICH9 watchdog MMIO region detected
- HPET watchdog capability detected
- Watchdog timer initialized with default timeout (10s)
- Watchdog is not running immediately after init

### NMI Handler — test_watchdog_nmi.cpp
- NMI pre-timeout handler installed
- NMI fires at TCO_RLD/2 (half of timeout)
- NMI handler logs warning and kicks watchdog
- NMI handler does not panic

### Watchdog Kick — test_watchdog_kick.cpp
- `SYS_WATCHDOG_KICK` resets the timer
- Kick before timeout prevents system reset
- Missing kick causes watchdog reset (in QEMU, verified by exit code)
- Invalid kick argument returns EINVAL

### PIT Fallback — test_watchdog_pit.cpp
- When no hardware watchdog, PIT-based software watchdog used
- PIT watchdog timeout matches requested period
- Software watchdog kick works identically
- Mixed hardware+software: hardware preferred

### Watchdog Create — test_task_watchdog_create.cpp
- `SYS_WATCHDOG_CREATE(period_ms)` returns valid watchdog ID
- Invalid period (0 or > 1h) returns EINVAL
- Maximum watchdogs per task enforced (returns ENOSPC)
- Watchdog is not armed on creation (explicit kick needed)

### Watchdog Kick — test_task_watchdog_kick.cpp
- `SYS_WATCHDOG_KICK(id)` resets timer for specific watchdog
- Expired watchdog (no kick within period) triggers action
- Kick on invalid watchdog ID returns EINVAL
- Kick on already-fired watchdog returns ESRCH

### Auto-Termination — test_task_watchdog_terminate.cpp
- Expired watchdog terminates the task
- Termination is clean (resources freed)
- Parent receives SIGCHLD with correct exit code
- Task in critical section (spinlock held) terminates safely

### /proc Export — test_watchdog_proc.cpp
- `/proc/[pid]/watchdog` lists all watchdogs for task
- Output format: `ID period_ms remaining_ms status`
- Status reflects: ARMING, ARMED, FIRED, DISABLED
- Reading /proc/[pid]/watchdog for another task returns EACCES

### Wait-For-Graph — test_wfg.cpp
- WFG constructed from locked resources and blocked tasks
- Single dependency: A waits for B -> directed edge A->B
- WFG updated atomically on lock/unlock
- WFG prunes resolved dependencies

### Deadlock Detection — test_deadlock_detect.cpp
- Two-task circular wait detected as deadlock
- Three-task cycle (A waits B, B waits C, C waits A) detected
- Nested mutex deadlock detected
- No false positive on valid lock ordering
- Detection completes in bounded time (O(tasks + resources))
- Detection overhead < 100 us per check

### Recovery — test_deadlock_recovery.cpp
- Deadlocked task forcibly terminated
- Terminated task's locks released
- Waiters of terminated task unblocked
- Resources of terminated task reclaimed
- Survivors continue without corruption
- Same deadlock not immediately re-formed after recovery

### Health Status — test_health.cpp
- `SYS_HEALTH_STATUS` returns system health metrics
- Fields: deadlock_count, recovered_count, watchdog_firings
- Health counter monotonically increasing
- Privileged-only: unprivileged caller gets EACCES
- `/proc/health` exposes same data in human-readable format

### RAM March Test — test_ram_march.cpp
- Idle task executes non-destructive March C- over unused RAM regions
- 0x55 pattern written and verified per address
- 0xAA pattern written and verified per address
- Original data restored byte-for-byte after test
- March test on used memory regions is skipped (no corruption)
- Single-bit error is detected and logged
- Multi-bit error (two cells flipped) is detected and logged
- Error counter exposed via `/proc/health` or `SYS_HEALTH_STATUS`
- Test completes within bounded time per sweep cycle

### CPU Register Verification — test_cpu_verify.cpp
- Idle task executes ALU test: (a + b) - b == a for randomised 64-bit operands
- Carry/overflow flags produce expected results for edge cases (0, MAX_U64, 1)
- Multiplication/division: a * b / b == a (for b ≠ 0)
- MSR read-back test: write known pattern to a scratch MSR, read back matches
- Non-scratch MSRs are read-only verified (write skipped)
- All general-purpose registers (RAX, RBX, RCX, RDX, RSI, RDI, R8–R15) exercised
- Flags register (RFLAGS) verified: set each flag, read back, clear, read back
- Verification failure triggers health counter increment and log entry
- Verification skipped during high system load (no latency impact)
