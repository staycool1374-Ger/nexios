# Configuration Matrix & `nexios_config.h` Specification

**Semantics:** binding contract for the kernel's compile-time configuration
surface — the `src/kernel/nexios_config.h` tunables, their categories,
valid ranges and cross-dependencies — and for the two config-matrix runners
(`run_config_matrix.sh`, `tools/deadline_matrix.sh`) that sweep the
deadline/WCET feature set, plus the `tools/check-config.py` validator.

```
 src/kernel/nexios_config.h   (single source of truth for CONFIG_*)
        │  #define CONFIG_<TUNER> <value>  (arch-gated #if blocks)
        ▼
 tools/check-config.py  ── validates range / cross-dependency / arch per tuner
        ▼
 make build  (NO_LTO / debug / release) ── the chosen matrix row is baked in
        ▼
 matrix runners (run_config_matrix.sh / tools/deadline_matrix.sh)
        │  set CONFIG_DEADLINE_* + CONFIG_SPORADIC_*  → clean build → execute-test
        ▼
 verdict: PASS / FAIL / HANG([HOST-WATCHDOG]) / expected-PANIC(PASS)
        ▼
 failures appended to BUGS.md  ("## Config-Matrix Bugs – <date>")
```

## 1. `nexios_config.h` — Configuration Surface

The header is organized in arch-gated `#if` blocks (x86_64 / aarch64 / riscv64),
each defining the shared tunables plus arch-specific ones.  Categories:

### 1.1 Build & Version
| Tuner | Default | Notes |
|---|---|---|
| `CONFIG_VERSION` | `"0.3.8-dev"` | version string |
| `CONFIG_VERSION_MAJOR/MINOR/PATCH/NUM` | 0/3/8/… | numeric for `CONFIG_SYSCALL_COUNT` versioning |

### 1.2 Architecture Capability Flags
| Tuner | x86_64 | aarch64 | riscv64 |
|---|---|---|---|
| `CONFIG_PAGE_SIZE` | 4096 | 4096 | 4096 |
| `CONFIG_HHDM_OFFSET` | `0xFFFF800000000000` | same | `0xFFFFFFC000000000` |
| `CONFIG_USER_SPACE_LIMIT` | `0x00007FFFFFFFFFFF` | `0x0000FFFFFFFFFFFF` | `0x000000FFFFFFFFFF` |
| `CONFIG_HAS_FPU` | 1 | 1 | 1 |
| `CONFIG_HAS_*` | RDRAND, APIC | GIC | PLIC, SBI |
| `CONFIG_HAS_MPU` / `CONFIG_HAS_HPET` | 0 | 0 | 0 |

**Rule (check-config enforces):** exactly one `CONFIG_ARCH_*` is active per
build; `CONFIG_PAGE_SIZE`/`CONFIG_HHDM_OFFSET`/`CONFIG_USER_SPACE_LIMIT` must
match the active arch block.

### 1.3 Scheduler & Tasks
| Tuner | Default | Range/Notes |
|---|---|---|
| `CONFIG_MAX_TASKS` | 64 | hard cap on live tasks |
| `CONFIG_TICK_HZ` | 1000 | timer ISR rate |
| `CONFIG_PRIORITY_CEILING` | 127 | highest legal priority |
| `CONFIG_MAX_PRIORITY` | 128 | == CEILING+1 (bitmap words) |
| `CONFIG_PREEMPTION` | 1 | 0 = cooperative-only |
| `CONFIG_TIME_SLICING` | 1 | round-robin at equal priority |
| `CONFIG_IDLE_YIELD` | 0 | |
| `CONFIG_SPORADIC_SERVER_MAX_TASKS` | 8 | |
| `CONFIG_SPORADIC_SERVER_BUDGET_GRANULARITY` | 1 | |

### 1.4 Memory / Address Space
`CONFIG_PML4_USER_COUNT` (256) · `CONFIG_STACK_SIZE` (65536) ·
`CONFIG_MIN_STACK_SIZE` (4096) · `CONFIG_HEAP_SIZE` (16 MiB) ·
`CONFIG_MAX_FDS` (32) · `CONFIG_MAX_MOUNTS` (32) · `CONFIG_MAX_DRIVERS` (16) ·
`CONFIG_MAX_DAEMONS` (16) · `CONFIG_MAX_PROGRAMS` (32) ·
`CONFIG_BUFFER_POOL_PAGES` (128) · `CONFIG_MAX_PROCESS_PAGES` (512) ·
`CONFIG_PAGE_TABLE_POOL_SIZE` (4096) · `CONFIG_VFS_MAX_PATH` (256) ·
`CONFIG_TASK_NAME_LEN` (16) · `CONFIG_MEMPOOL_NUM_POOLS` (9) ·
`CONFIG_MEMPOOL_BLOCK_SIZES` (16..4096) · `CONFIG_MEMPOOL_BLOCK_COUNTS`
(256..1).

### 1.5 IPC & Sync
`CONFIG_IPC_MAX_MSG_SIZE` (64 — fixed; the vfsd `Msg` is exactly this) ·
`CONFIG_IPC_MAX_QUEUE_MSG` (16) · `CONFIG_IPC_PRIORITY_LEVELS` (32) ·
`CONFIG_IPC_SHMEM_MAX_PAGES` (64) · `CONFIG_SYNC_MAX_WAITERS` (32) ·
`CONFIG_PRIORITY_CEILING_PROTOCOL` (1) · `CONFIG_MAX_HELD_CEILINGS` (16) ·
`CONFIG_MUTEX_PIP` / `CONFIG_SEMAPHORE_PIP` / `CONFIG_QUEUE_PIP` (1).

### 1.6 Syscall Inclusion (`CONFIG_INCLUDE_SYS_*`)
One tuner per syscall (YIELD, EXIT, FORK, CLONE, EXECVE, WAITPID, NANOSLEEP,
GETPID/GETPPID, SETPRIO/GETPRIO, SENDSYNC, RECV, REPLY, NOTIFY[_WAIT],
EVENT_POST/WAIT, BRK, MMAP/MUNMAP, OPEN..RMDIR, RENAME, MOUNT/UMOUNT, REBOOT,
HALT).  `CONFIG_SYSCALL_COUNT` aggregates them — **changing any `INCLUDE_SYS`
tuner changes the syscall table size, so all numbers `CONFIG_SYSCALL_COUNT`
feeds must be re-validated** (check-config cross-check).

### 1.7 Kernel Stacks & Guards
`CONFIG_KSTACK_WINDOW_BASE` (`0xFFFF900000000000`) ·
`CONFIG_KSTACK_WINDOW_SIZE` (16 MiB) · `CONFIG_STACK_SIZE_TABLE`
(8 size tiers) · `CONFIG_STACK_OVERFLOW_HOOK` (0).

### 1.8 Interrupts
`CONFIG_USE_APIC_TIMER` (1) · `CONFIG_THREADED_IRQS` (1) ·
`CONFIG_MAX_THREADED_IRQS` (16) · `CONFIG_IRQ_LATENCY_HISTOGRAM` (1) ·
`CONFIG_IRQ_LATENCY_MAX_NS` (0) · `CONFIG_PREEMPTION_LATENCY_MAX_CYCLES` (0).

### 1.9 Hooks & Policies
`CONFIG_IDLE_HOOK` / `CONFIG_TICK_HOOK` / `CONFIG_INIT_HOOK` (0) ·
`CONFIG_OOM_HOOK` (0) · `CONFIG_OOM_POLICY` (0) ·
`CONFIG_STATIC_POOLS_ONLY` (0) · `CONFIG_MEMORY_BUDGET` (0) ·
`CONFIG_ZOMBIE_STARVATION_LIMIT` (32).

### 1.10 Deadline / WCET Feature Set (the matrix dimensions)
| Tuner | Default | Meaning |
|---|---|---|
| `CONFIG_SPORADIC_SERVER_DEADLINE_HOOK` | 1 | SS budget exhaustion feeds the deadline hook |
| `CONFIG_DEADLINE_MISS_DETECTION` | 1 | enable `scan_deadlines`/detection |
| `CONFIG_WCET_OVERRUN_DETECTION` | 1 | enable WCET overrun handler |
| `CONFIG_SPORADIC_SERVER_EXHAUSTION_IS_DEADLINE` | 0 | treat SS exhaustion as a deadline miss |
| `CONFIG_DEADLINE_MONITOR_TASK` | 1 | spawn the prio-127 monitor task |
| `CONFIG_DEADLINE_ACTION` | 0 | miss action (compile-time dispatch) |
| `CONFIG_DEADLINE_MONITOR_PID` | 0 | NOTIFY_MONITOR target |

**`CONFIG_DEADLINE_ACTION` — compile-time dispatch (see `specs/deadline.md` §3):**
```
0 LOG_ONLY (default)   1 PANIC   2 DEMOTE (prio>>=1+move_priority)
3 KILL (defer_kill)    4 NOTIFY_MONITOR (SIGUSR1)
```

### 1.11 Capabilities (CSpace) — v0.4.1
| Tuner | Default | Meaning |
|---|---|---|
| `CONFIG_CSLOT_COUNT` | 64 | slots per CNode (the task's root CNode is its CSpace); power of two preferred — slot-index bits are carved from the capability handle |
| `CONFIG_CAP_MAX_DEPTH` | 8 | cascade-revoke depth bound; the revoke walk is iterative (explicit work list), never unbounded recursion |
| `CONFIG_CAP_MAX_UNTYPED` | 16 | live Untyped memory objects (TU-local counter in untyped.cpp) |
| `CONFIG_CAP_MAX_MMIO` | 16 | live MmioCap objects (TU-local counter in mmio.cpp; issue #3) |
| `CONFIG_CAP_MAX_IRQ` | 16 | live IrqCap objects + IRQ delivery-table slots (TU-local counter in irq.cpp; static table in irq_delivery.cpp, no RT-path allocation; issue #2) |
| `CONFIG_CAP_MAX_MSIX` | 8 | live MsixCap objects (TU-local counter in cap/msix.cpp; per-(BDF,entry) single-owner claim registry; shares the IRQ delivery table, issue #10) |
| `CONFIG_IOPB_MAX_TASKS` | 4 | per-task TSS I/O-bitmap pool slots (x86_64 sys_ioport_grant; static 8 KiB×N .bss, no RT-path allocation; issue #3) |
| `CONFIG_CAP_MAX_IOMMU` | 16 | live IoMmuDmaCap objects (TU-local counter in cap/iommu.cpp; issue #4) |
| `CONFIG_IOMMU_MAX_DOMAINS` | 8 | concurrent IOMMU DMA domains (static table in iommu.cpp; each domain owns one zeroed PMM SL-root page; issue #4) |
| `CONFIG_IOMMU_MAX_MAPPINGS` | 32 | outstanding DMA mappings per domain (bounded record table; exhausting fails closed; issue #4) |
| `CONFIG_IOMMU_MAX_BUSES` | 8 | PCI buses with a static IOMMU context table (256×16 B .bss each; issue #4) |

## 2. The Matrix Runners

### 2.1 `run_config_matrix.sh` — 48 meaningful deadline/WCET combos

The 5-dimensional sweep: `MONITOR × MISS × WCET × SPORADIC × ACTION` where
`ACTION` is only meaningful when `MISS==1`:

```
per (MONITOR, WCET, SPORADIC):          # 2 × 2 × 2 = 8 families
    MISS=0, ACTION=0                    #  1 combo (action irrelevant)
    MISS=1, ACTION ∈ {0,1,2,3,4}        #  5 combos
                                        # = 6 per family → 8 × 6 = 48 total
```

Per combo (`run_one`):
1. `restore_config` (from `jarvis_config.h.matrix_backup`),
   then `sed`-set the five `CONFIG_DEADLINE_*`/`CONFIG_SPORADIC_*` defines.
2. `make clean` + `make -jN`.
3. `make execute-test x86_64 debug all TEST_SERIAL_LOG=$log`.
4. **Verdict:** exit≠0 AND `[HOST-WATCHDOG]` in log → **HANG**; exit≠0 → **FAIL**;
   else **PASS**.
5. Success logs purged; failures appended to `BUGS.md`
   (`## Config-Matrix Bugs – <date>`).

**KNOWN ISSUE:** the script targets `src/kernel/jarvis_config.h`, which does
**not exist** — the real header is `src/kernel/nexios_config.h`.  As written the
`sed`/`restore_config` steps no-op or fail, so the matrix does not actually
reconfigure the build.  **Fix required:** point `CONFIG_FILE` at
`src/kernel/nexios_config.h` (and prefer `CONFIG_DEFS="-D..."` like
`tools/deadline_matrix.sh` to avoid editing the tracked header).

### 2.2 `tools/deadline_matrix.sh` — CONFIG_DEADLINE_ACTION sweep

Iterates `ACTION ∈ {0..4}` via `make CONFIG_DEFS="-DCONFIG_DEADLINE_ACTION=$action"`
and runs the `deadline_action` class.  **PANIC (action=1) is an expected-fail**:
the kernel must halt and print `action=PANIC`; its presence is the PASS signal.
Other actions must pass the class normally.  Kills leftover QEMU between runs.

## 3. `tools/check-config.py` — Configuration Validation

- Parses `nexios_config.h` (crude `#if/#elif/#else/#endif` preprocessor +
  `defined()`/integer expression evaluator); seeds arch flags from `--arch` /
  `ARCH` env / host detection.
- Validates per tuner: documented range, cross-dependencies, arch constraints.
- Exit codes: **0** pass, **1** errors, **2** warnings.  Wired into
  `make check-config` / the build gate.

**Validation rules that must hold for any matrix row:**
1. Exactly one `CONFIG_ARCH_*` active; `PAGE_SIZE`/`HHDM_OFFSET`/
   `USER_SPACE_LIMIT` match it.
2. `CONFIG_DEADLINE_ACTION` in [0,4]; it is only meaningful when
   `CONFIG_DEADLINE_MISS_DETECTION==1`.
3. `CONFIG_PRIORITY_CEILING` ≤ 127; `CONFIG_MAX_PRIORITY` == CEILING+1.
4. `CONFIG_BUFFER_POOL_PAGES` == `BufferPool::POOL_PAGES` (specs/memory.md §5).
5. `CONFIG_IPC_MAX_MSG_SIZE` ≥ `sizeof(vfsd::Msg)` (64, fixed).
6. `CONFIG_SYSCALL_COUNT` consistent with the enabled `CONFIG_INCLUDE_SYS_*`.
7. `CONFIG_THREADED_IRQS` ⇒ `CONFIG_MAX_THREADED_IRQS` ≥ active vectors.

## 4. Matrix Semantics (what each dimension changes at runtime)

| Dimension | Runtime effect |
|---|---|
| `CONFIG_DEADLINE_MONITOR_TASK=0` | no monitor task; `on_tick` runs the inline `deadline_list_.pop_earliest_if_expired()` loop |
| `CONFIG_DEADLINE_MISS_DETECTION=0` | `scan_deadlines` compiled out; no miss handler |
| `CONFIG_WCET_OVERRUN_DETECTION=0` | WCET overrun handler compiled out |
| `CONFIG_SPORADIC_SERVER_DEADLINE_HOOK=0` | SS budget exhaustion no longer feeds the deadline path |
| `CONFIG_DEADLINE_ACTION` | selects the miss action at compile time (§1.10) |

## 5. Open Items

- **Fix `run_config_matrix.sh` path** (`jarvis_config.h` → `nexios_config.h` /
  `CONFIG_DEFS`).
- **A release-gate constraint applies to every row:** `CONFIG_DEBUG_IPC_SCHED`
  must stay **undefined** for release gates (see `specs/deadline.md` §7); the
  matrix runs `debug` with the trace OFF.
- **`CONFIG_DEADLINE_ACTION=1 (PANIC)` is the only expected-fail row** — the
  other 47 must pass.
