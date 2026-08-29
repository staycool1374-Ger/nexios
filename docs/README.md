# Jarvis RTOS — Technical Documentation

**Semantics:** the docs tree is a set of **binding specifications** extracted
from the design/audit papers.  Historical root-cause analyses, superseded
plans, and fully-fixed fix-records live in `_archive/` (source material,
retained for traceability).  Open issues are tracked in `ROADMAP.md` →
"Open Issues".

## Spec Documents

| Doc | Semantic (what it binds) |
|---|---|
| `specs/scheduler.md` | scheduler / ready-queue / priority / task-lifecycle contracts (R1-3, INV-1..7, move_priority, WEDGE, snapshot rebuild) |
| `specs/ipc.md` | message-queue IPC, `send_sync`, mutex/PCP, and the deferred-switch machinery contracts |
| `specs/memory.md` | PMM / MemPool / VMM / kstack guard / snapshot-isolation contracts + REQ-MP-01..06 |
| `specs/boundary.md` | syscall / VFS / ELF trust-boundary contracts (6 principles, VULN ledger) |
| `specs/oom-rt.md` | OOM admission control, allocation-failure contract, WCET bounding |
| `specs/vfs.md` | VFS subsystem: vnode model, mount model, vfsd protocol, path resolution + TOCTOU closure, FdTable, filesystem backends |
| `specs/deadline.md` | deadline model, `scan_deadlines`, deadline-monitor task (dangling-pointer hazard), WCET overrun, miss actions |
| `specs/test-harness.md` | driven-test discipline (INV-4 cookbook), snapshot isolation, ResourceTracker, registry/class system, watchdog |
| `specs/drivers.md` | block-device abstraction, AHCI/ATA-PIO/virtio-blk, DMA contract, interrupt dispatch, binding invariants + open FLAW ledger |
| `specs/configuration.md` | `nexios_config.h` tunables (categories, ranges, arch gates), the 48-combo `run_config_matrix.sh` sweep, `tools/deadline_matrix.sh`, `tools/check-config.py` validator, matrix semantics |
| `zombie-list-spec.md` | zombie / reaper lifecycle detail (referenced by `specs/scheduler.md` §6) |
| `debugging.md` | GDB/lldb tooling workflow (operational, not a spec) |

## Relationship Map

```
                    ┌──────────────────────────────────────┐
                    │          ROADMAP.md (active)          │
                    │  Open Issues · v0.3.12 · Future 0.4+ │
                    └───────┬──────────────────────┬───────┘
                            │                      │
        ┌───────────────────┼──────────────────────┼───────────────────┐
        ▼                   ▼                      ▼                   ▼
 ┌───────────────┐   ┌───────────────┐      ┌──────────────┐   ┌──────────────┐
 │ specs/        │   │ specs/        │      │ specs/       │   │ specs/       │
 │ scheduler.md  │◀──│ ipc.md        │───▶  │ memory.md    │◀──│ boundary.md  │
 │ (ready queue, │   │ (IPC/sync,    │       │ (PMM/VMM/    │   │ (syscall/VFS/│
 │  priority,    │   │  deferred-sw) │       │  stack/snap) │   │  ELF)        │
 │  lifecycle)   │   └──────┬────────┘       └──────┬───────┘   └──────┬───────┘
 └──────┬────────┘          │                      │                    │
        │                   │        ┌─────────────┴─────┐             │
        ▼                   ▼        ▼                   ▼             ▼
 ┌──────────────┐   ┌──────────────┐   ┌───────────────────────┐  ┌──────────────┐
 │ zombie-list  │   │ oom-rt.md    │   │ v0.3.12 audit         │  │ debugging.md │
 │ spec         │   │ (admission,  │   │ (alloc/free return-   │  │ (GDB/lldb)   │
 └──────────────┘   │  WCET)       │   │  value audit, A1-A4)  │  └──────────────┘
                    └──────────────┘   └───────────────────────┘

 New (gap-closing) specs:
 ┌───────────────┐  ┌────────────────┐  ┌─────────────────────┐  ┌──────────────┐
 │ specs/vfs.md  │  │ specs/deadline │  │ specs/test-harness  │  │ specs/       │
 │ (vnode/mount, │  │ (scan_dead-    │  │ (driven discipline, │  │ drivers.md   │
 │  vfsd proto,  │  │  lines, monitor│  │  snapshot, Resource-│  │ (block dev,  │
 │  TOCTOU)      │  │  task, WCET)   │  │  Tracker, watchdog) │  │  AHCI/DMA,   │
 └───────────────┘  └────────────────┘  └─────────────────────┘  │  interrupts)  │
                                                                 └──────┬───────┘
 ┌──────────────────────┐                                          ┌──────┴───────┐
 │ specs/configuration  │── feeds/validates ─▶ nexios_config.h     │              │
 │ (nexios_config.h +   │◀── check-config.py, run_config_matrix,   │              │
 │  matrix runners)     │    deadline_matrix.sh                    │              │
 └──────────────────────┘                                          │              │
```

```
 Caller / cross-reference summary (who reads whom):

 Scheduler (specs/scheduler.md)
   ├─ reads: zombie-list-spec.md (termination)   specs/ipc.md (send_sync RQ rows)
   ├─ read-by: specs/ipc.md (deferred-switch), specs/memory.md (snapshot RQ)
   ├─ read-by: specs/deadline.md (INV-6 move_priority), specs/test-harness.md (INV set)
   └─ read-by: ROADMAP_done.md (H2, ss_deadline — both resolved)

 IPC (specs/ipc.md)
   ├─ reads: specs/scheduler.md (INV-5, move_priority)
   ├─ read-by: specs/boundary.md (VULN-W2/W3 blocking), specs/oom-rt.md (WCET)
   └─ read-by: specs/vfs.md (send_sync authorization), specs/test-harness.md (blocking)

 Memory (specs/memory.md)
   ├─ reads: specs/scheduler.md (snapshot rebuild), specs/oom-rt.md (budget)
   └─ read-by: specs/boundary.md (W^X map_page), specs/ipc.md (pool/owner),
               specs/drivers.md (DMA buffers)

 VFS (specs/vfs.md)
   ├─ reads: specs/ipc.md (send_sync), specs/boundary.md (CheckedPtr)
   └─ read-by: specs/test-harness.md (g_vfs_touched), specs/oom-rt.md (WCET envelope)

 Deadline (specs/deadline.md)
   ├─ reads: specs/scheduler.md (priority/INV-6), specs/oom-rt.md (WCET)
   ├─ read-by: specs/test-harness.md (trigger_deadline_monitor_scan)
   └─ read-by: specs/configuration.md (CONFIG_DEADLINE_ACTION dispatch)

 Drivers (specs/drivers.md)
   ├─ reads: specs/memory.md (DMA/contiguous), specs/scheduler.md (wake)
   └─ audit: audits/hardware_ahci.md (FLAW ledger)

 Configuration (specs/configuration.md)
   ├─ reads: specs/deadline.md (action dispatch), specs/memory.md (pool pages)
   └─ governs: nexios_config.h via check-config.py + the matrix runners
```

## Source Material (archived)

`_archive/` holds the original papers the specs were extracted from, including
the master investigation logs (`investigation-cumulative-corruption.md`,
`ipc_blocking-analysis.md`) and the `ipc_blocking-c-baseline.log` watchdog dump.
They are retained for traceability and are **not** normative.

## Benchmarks / Generated

- `benchmarks/mandelbrot.md` — benchmark results.
- `doxygen/html/` — regenerable Doxygen output (67 MB, tool-generated).
