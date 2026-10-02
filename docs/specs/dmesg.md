# dmesg — Structured Kernel Log (issue #234)

**Status:** IMPLEMENTED — severity taxonomy for every error code,
canonical per-subsystem numbering, central catalog, single renderer,
boundary emission.
**Owner:** kernel core (log subsystem)
**Sources:** `src/kernel/log/dmesg.{hpp,cpp}`,
`src/kernel/log/dmesg_catalog.hpp`, `src/kernel/task/dmesg_task.cpp`

---

## 1. Severity semantics

Severity is stored per entry at push time (never re-derived by the
renderer — re-derivation risks masking faults as INFO):

| Severity | Meaning | Example |
|---|---|---|
| INFO | Action succeeded, confirmational | ELF completed, daemon restarted, daemons ready |
| WARN | Unusual/degraded/routine-negative, system stable | ARP failed, admission denied, queue empty, rc missing |
| ERROR | Operation failed, system handled it via an action | Daemon died (restarted), deadline missed, probe failed |
| FATAL | Unrecoverable/integrity violation, halt or kill | Authenticity kill, kernel panic, TCB corruption |
| DEBUG | Verbose/trace, debug targets only | State-machine dumps (CONFIG_DEBUG-gated, never in ring) |

Taxonomy rule (`catalog::lookup_severity`, single truth): catalog event
records first; code 0 (OK) → INFO; BASE legacy event ranges keep historic
INFO via `base_code_is_info`; explicitly listed routine codes → WARN
(see §3 per-table WARN lists); everything else fails closed to ERROR.
Only explicit FATAL records ever render FATAL.

## 2. Codebase identifiers and canonical numbers

Each subsystem owns `[base, base + 1000)` (`kDmesgStride = 1000`).
Raw table codes and canonical numbers (`base + raw`) decode identically
(`strip_canonical`); catalog event records take precedence over tables.

| Codebase | Base | Code space |
|---|---|---|
| BASE | 0 | `kernel::Error` 0–9 + legacy events + `kDmesgPanicCode` 0xFA57 |
| SYNC | 1000 | `SyncError` 0–14; WARN: 3, 7, 9, 12, 13, 14 |
| VFS | 2000 | `VfsError` 0–15; WARN: 3, 4, 5, 6, 7 |
| MPOOL | 3000 | `MemPoolError` 0–7; all non-zero ERROR |
| SCHED | 4000 | `SchedulerError` 0–14; WARN: 12 (admission denied) |
| IPC | 5000 | `IpcError` 0–9; WARN: 3 (queue empty) |
| SYSCALL | 6000 | mirror table 0–907; WARN: 403, 407, 409, 412–414, 503–507, 603 |
| NET | 7000 | events: 1 link-up INFO, 2 link-down WARN, 3 tx-timeout ERROR, 4 ARP-fail WARN, 5 pkt-too-large ERROR |
| ELF | 8000 | events 1–20 (see §4) |
| USER | 9000 | events: 1 exited INFO, 2 faulted ERROR, 3 signal-terminated ERROR, 4 started INFO |
| DAEMON | 10000 | events: 1 exited ERROR, 2 restarted INFO, 3 ensured INFO, 4 terminated INFO, 5 restarting INFO, 6 up INFO, 7 restart-limit ERROR, 8 restart-failed ERROR |
| PMM | 11000 | `PmmError` 0–4; all non-zero ERROR |
| VMM | 12000 | `VmmError` 0–4; all non-zero ERROR |
| TASK | 13000 | `TaskError` 0–9 (9 = TCB corruption, added #234); all non-zero ERROR |
| BUFPOOL | 14000 | `BufPoolError` 0–7 (3 skipped upstream); all non-zero ERROR |
| DRIVER | 15000 | `PciError` 0–4 (table) + events 1–10 (see §4) |
| INIT | 16000 | events 1–17 (see §4) |
| TIMING | 17000 | events 1–6 (see §4) |
| TEST | 18000 | events: 1 leak ERROR, 2 count-drift WARN |

## 3. Complete error-code list with severity

Notation: `SUBSYS:raw [canonical] "text" → SEV`.

BASE (`lib/error.hpp` + legacy + panic):
`0 "OK"→INFO, 1 "Out of memory"→ERROR, 2 "Invalid argument"→ERROR,
3 "Not found"→ERROR, 4 "Already exists"→ERROR, 5 "Timeout"→ERROR,
6 "Busy"→ERROR, 7 "Not implemented"→ERROR, 8 "I/O error"→ERROR,
9 "Corrupted"→ERROR, 0xDA01–0xDA06 daemon events→(legacy INFO-range),
0xDB01–0xDB0D ELF events→(legacy INFO-range),
0xDC01–0xDC02 task-end→(legacy INFO-range),
0xFA57 "Kernel panic"→FATAL (pushed explicitly).`

SYNC (`sync_errors.hpp`):
`0 OK→INFO, 1 No current task→ERROR, 2 Max waiters→ERROR,
3 Already initialized→WARN, 4 Not owner→ERROR, 5 Not locked→ERROR,
6 Queue full→ERROR, 7 Queue empty→WARN, 8 Msg too large→ERROR,
9 Already waiting→WARN, 10 Invalid args→ERROR, 11 Buffer full→ERROR,
12 Buffer empty→WARN, 13 No waiter→WARN, 14 Interrupted→WARN.`

VFS (`vfs_errors.hpp`):
`0 OK→INFO, 1 Invalid FD→ERROR, 2 FD table full→ERROR, 3 Not found→WARN,
4 Not a directory→WARN, 5 Is a directory→WARN, 6 Exists→WARN,
7 Not empty→WARN, 8 No device→ERROR, 9 No space→ERROR, 10 Permission→ERROR,
11 Invalid args→ERROR, 12 I/O error→ERROR, 13 Not supported→ERROR,
14 Mount busy→ERROR, 15 No such FS→ERROR.`

MPOOL (`mempool_errors.hpp`):
`0 OK→INFO, 1 OOM→ERROR, 2 Too large→ERROR, 3 Invalid ptr→ERROR,
4 Double free→ERROR, 5 Uninit→ERROR, 6 Invalid pool→ERROR,
7 Corrupted→ERROR.`

PMM (`pmm_errors.hpp`):
`0 OK→INFO, 1 OOM→ERROR, 2 User OOM→ERROR, 3 Table OOM→ERROR,
4 Invalid addr→ERROR.`

VMM (`vmm_errors.hpp`):
`0 OK→INFO, 1 Page alloc→ERROR, 2 PML4 alloc→ERROR, 3 Invalid addr→ERROR,
4 Not mapped→ERROR.`

SCHED (`scheduler_errors.hpp`):
`0 OK→INFO, 1 Table full→ERROR, 2 Not found→ERROR, 3 Duplicate ID→ERROR,
4 Invalid state→ERROR, 5 No current→ERROR, 6 No idle→ERROR,
7 Preempt disabled→ERROR, 8 Zombie→ERROR, 9 Invalid magic→ERROR,
10 No shell→ERROR, 11 Invalid args→ERROR, 12 Admission denied→WARN,
13 WCET invalid→ERROR, 14 Remote-current→ERROR.`

TASK (`task_errors.hpp`):
`0 OK→INFO, 1 OOM→ERROR, 2 Table full→ERROR, 3 Stack alloc→ERROR,
4 Ustack alloc→ERROR, 5 PML4 clone→ERROR, 6 Not found→ERROR,
7 Invalid arg→ERROR, 8 Invalid state→ERROR,
9 TCB corrupt→ERROR (pushed as FATAL at the remove_task site).`

IPC (`ipc_errors.hpp`):
`0 OK→INFO, 1 No queue→ERROR, 2 Queue full→ERROR, 3 Queue empty→WARN,
4 No dest→ERROR, 5 No reply→ERROR, 6 Timeout→ERROR, 7 Invalid msg→ERROR,
8 No buffer→ERROR, 9 Invalid args→ERROR.`

BUFPOOL (`buffer_pool_errors.hpp`):
`0 OK→INFO, 1 OOM→ERROR, 2 Max buffers→ERROR, 4 Not owner→ERROR,
5 VA in use→ERROR, 6 VA out of range→ERROR, 7 Not mapped→ERROR.`

DRIVER table (`pci_errors.hpp`):
`0 OK→INFO, 1 No dev at BDF→ERROR, 2 PCI alloc fail→ERROR,
3 Invalid BAR→ERROR, 4 Dev not found→ERROR.`

SYSCALL mirror (`syscall_errors.hpp`): TASK 101–109, PMM 201–204,
VMM 301–304, SYNC 401–414 (WARN: 403, 407, 409, 412, 413, 414),
VFS 501–515 (WARN: 503–507), IPC 601–609 (WARN: 603), BUF 701–707,
SCHED 801–811, MEMPOOL 901–907; 0–3 base codes ERROR except 0→INFO.

## 4. Event records (emission catalogue)

ELF 8001–8015 as before, plus: 8016 segment-past-heap ERROR,
8017 no-red-zone ERROR, 8018 heap-overrun ERROR, 8019 shared null-arg ERROR,
8020 image-not-in-closure ERROR.
NET 7004 ARP-fail WARN, 7005 pkt-too-large ERROR.
DAEMON 10007 restart-limit ERROR, 10008 restart-failed ERROR.
USER 9003 signal-terminated ERROR, 9004 task-started INFO (rc loop +
runelf activation).
DRIVER 15001 probe-fail ERROR, 15002 transport-fail ERROR,
15003 queue-setup-fail ERROR, 15004 request-timeout ERROR,
15005 request-failed ERROR, 15006 OOM ERROR, 15007 feature-reject ERROR,
15008 DMA-fail ERROR, 15009 fallback-config WARN, 15010 scan-truncated WARN.
INIT 16001 rc-args WARN, 16002 rc-missing WARN, 16003 rc-invalid ERROR,
16004 rc-not-exec ERROR, 16005 rc-path-long WARN, 16006 rc-not-accepted ERROR,
16007 rc-load-failed ERROR, 16008 rc-policy ERROR, 16009 rc-admission ERROR,
16010 daemon-timeout ERROR, 16011 daemon-missing ERROR,
16012 shell-create-fail ERROR, 16013 taskdef-invalid ERROR,
16014 taskdef-create-fail ERROR, 16015 fstab-mount-fail ERROR,
16016 daemons-ready INFO, 16017 shell-started INFO.
TIMING 17001 deadline-missed ERROR, 17002 budget-exhausted ERROR,
17003 admission-denied WARN, 17004 liu-leyland-exceed WARN, 17005 wcet-overrun WARN,
17006 watchdog-expired ERROR.
TEST 18001 leak-detected ERROR, 18002 count-drift WARN.

## 5. Emission inventory (who pushes what)

Scheduler: deadline/budget (TIMING 1/2 ERROR), WCET (TIMING 5 WARN),
watchdog expiry (TIMING 6 ERROR, issue #41),
admission incl. affinity partition (TIMING 3 WARN), Liu-Leyland
(TIMING 4 WARN, first exceed only — the check fires per admission and
would flood the ring; every occurrence stays on serial), TCB corruption
(TASK 9 FATAL), idle OOM (TASK 1 ERROR), deferred-kill full (SCHED 1
ERROR), terminate refusals (SCHED WARN).
Task: TCB stack budget/PMM OOM (TASK 1 ERROR).
Daemon: exit/restart/ensure/terminate (DAEMON 1–4), limit (DAEMON 7),
restart attempts (DAEMON 8).
Init: rc loop (INIT 1–9), daemon timeout/missing (INIT 10/11),
shell create (INIT 12), taskdefs (INIT 13/14), fstab mount (INIT 15),
milestones daemons-ready/shell-started (INIT 16/17 INFO).
ELF: post_event map (ELF 1–15), validate rejects (ELF 16–18),
shared rejects (ELF 19/20). Integrity: auth kill (ELF 14 FATAL).
User: clean/fault exits (USER 1/2).
Drivers: ahci timeouts/alloc/slot/port (DRIVER 1/4/5/6),
virtio transport/queue/OOM/timeout/fail/negotiation (DRIVER 2/3/4/5/6/7),
virtio-net RX OOM + MAC fallback (DRIVER 6 / 9 WARN),
virtio-pci map/feature/queue (DRIVER 2/7/3), dma alloc/map (DRIVER 8),
PCI scan truncate (DRIVER 10 WARN).
Memory: PMM window exhaust (PMM 1 ERROR), MemPool pinned-keep (MPOOL WARN).
Net: ARP fail (NET 4 WARN), oversize (NET 5 ERROR).
Test: leak (TEST 1 ERROR), count drift at registration (TEST 2 WARN).
Kernel: panic (BASE 0xFA57 FATAL, pushed before serial output).
Shell: runelf admission (TIMING 3 WARN).

Logger-only by design (no ring push): routine polling empties handled at
call sites (would flood the 4096-ring), CONFIG_DEBUG traces (debug-target
only per issue), auth progress-reset/kill-refused benign races, SMP
affinity clamp admin noise, per-test skip notices, test-harness internals
(fuzz/probe chatter). All remain on the serial console.

## 6. Legacy compatibility (old → new map)

Unchanged from the previous revision (0xDAxx→DAEMON 10001–10006,
0xDBxx→ELF 8001–8015 with the 0xDB08/0xDB09 producer-meaning correction,
0xDCxx→USER 9001/9002); `kElfAuthKillExitCode` stays 0xDB0C (exit-code ABI).

## 7. Render format

`[DMESG <time>]: <TYPE> <CODEBASE> <nr> <text>: <msg> [task=<id>
ctx=0x<hex>]`, single `format_dmesg_entry()` owned by log. `<time>` is the
wall-clock datetime `YYYY-MM-DD hh:mm:ss:mmm` whenever the boot epoch is
known, else `<ticks>ms tick` (early boot before the RTC is read).

## 8. Concurrency & safety contract

Unchanged: SPSC atomics, no locks (pushes are legal under spinlocks —
no reschedule inside), no allocation, bounded loops, debug/release parity.
Ring capacity 4096, overwrite-oldest: failure bursts cannot wedge the ring.
