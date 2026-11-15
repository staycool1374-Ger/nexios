# Microkernel Transition — Normative System Transformation Spec

**Milestone:** `v0.6.1 Transition to full Microkernel` (GitHub milestone #18).
The separate milestone `v0.7.0 Audit Specifications for Transition to
Microkernel` (#20) is a different milestone and is not the target of this spec.
**Phase:** 8 (microkernel transition).
**Anchor issues:** #512 (plan), #547 (evidence audit).
**Component issues:** #190, #191, #192, #193, #194, #516, #517, #476, #545,
#546, #49, #50, #398, #515; closed predecessors #47, #48, #51, #52, #53, #54,
#76, #105.
**Milestone gap / coverage issues:** #528 (aarch64 ioport/IOMMU dispatch
tests, §11), #544 (riscv64 debug-syscall flake, KEEP-D 86–90, §11), #89
(v0.6.1 test coverage tracker, §13).

## 0. Scope, language, evidence rules

### 0.1 Language

MUST / MUST NOT / SHALL / SHALL NOT are normative. "Should" is advisory.
A value written `TBD at API freeze` is an owner decision that has not been
made (see §16). No other unmeasured number is normative in this document.

### 0.2 Evidence rule

Every factual claim cites `path:symbol`. Line numbers are advisory and
SHALL NOT be treated as normative (they drift; prior audit
`audits/report-2026-09-19T22:14:29Z.md` S3 recorded one such drift).
Performance figures are allowed only when measured in this tree; external
literature figures are not requirements.

### 0.3 Scope

In scope: the kernel mechanism boundary, the invariants every component must
keep, the inter-server contracts, the failure and reclaim rules, the per-arch
parity matrix, and the gated migration sequence.

Out of scope: the scheduler core algorithm (O(1) bitmap + RMS/EDF,
`scheduler.md`), TCP/IP and USB stacks (Phase 9), new server internals beyond
their kernel-facing contracts.

R-SCOPE (owner, #512): internal implementation bugs are not an architecture
input. Fault containment (F6/M1 latch, supervision restart) and the contracts
below are defense-in-depth; no protocol assumes a buggy peer, and every
protocol survives one (R-INPUT validation, bounded calls, death wake).

### 0.4 Design references

The target follows three established patterns, used only as structure:
seL4 MCS (passive servers on donated scheduling contexts, timeout faults),
LionsOS / Microkit (static system description, strictly sequential servers,
SPSC shared-memory channels), MINIX 3 (reincarnation of failed servers).

### 0.5 Normative in-tree references

| Spec | Binds |
|---|---|
| `ipc.md`, `ipc-fastpath.md`, `syscall-fastpath.md` | message queues, deferred switch, FAST tier |
| `scheduler.md`, `rms-rework.md`, `deadline.md`, `wcet-reaudit-v0.4.5.md` | scheduler, breaker, budgets, WCET |
| `memory.md`, `oom-rt.md`, `configuration.md` | PMM/MemPool/VMM, budgets, CONFIG_* |
| `cspace.md`, `shm.md`, `iommu.md`, `external-pager.md` | capabilities, frames, DMA, pager |
| `death-notify.md`, `zombie-list.md`, `idle_monitor.md`, `watchdog.md` | death, teardown, supervision inputs |
| `irq-early-init.md`, `per-cpu-smp.md`, `threads.md` | IRQ init, per-CPU, threads |
| `driver-pds.md`, `block-srv.md`, `vfs-srv.md`, `timer-srv.md`, `term-srv.md` | existing PD contracts |
| `boundary.md`, `privilege-audit.md` | trust boundary, ring inventory |

## 1. Normative invariants

Each invariant states its enforcement point and its current status. An
invariant with a known violation is tagged `STATUS: VIOLATED` with the
tracking issue; it is never presented as already true.

| ID | Invariant | Enforcement point | Status / evidence |
|---|---|---|---|
| INV-MK-1 | After boot ownership handover the kernel SHALL NOT allocate physical memory on behalf of a task. Every kernel object SHALL be created by `CAP_RETYPE` from a user-held Untyped, except the fixed boot pool set (§7.2). | `CONFIG_STATIC_POOLS_ONLY` asserts in `memory/pmm.cpp:PMM::alloc_page` / `alloc_contiguous` / `alloc_page_colored`; GAP: `alloc_user_page`, `alloc_user_contiguous`, `alloc_page_table` carry no such assert, and every listed violation site uses them (F21) | VIOLATED (#193): `syscall_handlers_shm.cpp:sys_frame_create` (PMM), `ipc/buffer_pool.cpp:BufferPool::alloc_page` (PMM fallback), `elf/elf_loader.cpp` page allocation, `memory/vmm.cpp` page-table allocation on map |
| INV-MK-2 | Every scarce resource SHALL have a compile-time bound, and the composed system description SHALL be checked against it at compose time. | `task/boot_sdf.hpp:sdf_compose` | PARTIAL: bounds exist (§7.3); demand-vs-bound check absent |
| INV-MK-3 | Every client-visible server RPC SHALL have a finite, documented bound. Unbounded `send_sync` SHALL NOT appear on any server path. | proxy call sites | PARTIAL: proxies use `kRpcGuardTicks = 5000` (5 s at `CONFIG_TICK_HZ 1000`) in `log/log_srv.cpp`, `time/timer_srv.cpp`, `cap/cap_broker.cpp`, `term/term_srv.cpp`, `vfs/procfs_srv.cpp`, `vfs/vfs_srv.cpp`, `driver/block_srv.cpp`, `driver/driver_pds.hpp`, `watchdog/wdog_mon.cpp`; per-server bounds `TBD at API freeze` (Q5) |
| INV-MK-4 | Task context runs with IF=1 except inside an explicit `IrqGuard`; syscall entry owns IF=0 until the handler blocks or returns; no spinlock SHALL be held across a blocking point or `reschedule()`; every IRQ-off region SHALL be bounded. | debug assertions (§6) | VIOLATED (#545 S1, #546 S2); contract currently implicit |
| INV-MK-5 | A context switch is applied only at an ISR epilogue at nesting depth 1, or a nested timer (vector `0xE0`) at depth 2. A blocking IPC hop therefore costs up to one tick until a direct-switch path exists (§5.6). | `arch/x86_64/isr_stubs.asm` epilogue, `arch/aarch64/vectors.S`, `arch/riscv64/syscall_entry.S` + `interrupt_controller.cpp` (§11); `ipc.md` §4 | TRUE today; is the latency limit, removed by M3 |
| INV-MK-6 | Exactly one death record per task, latched at a single funnel, visible to a watching supervisor within a bounded number of ticks. | `ipc/death_notify.cpp:DeathNotify::on_task_death` | VIOLATED (#516 + F6): latch only in `TaskControlBlock::complete_teardown()` (reached via `cleanup()` / `destroy()`); for `Scheduler::terminate` deaths (kill, fault, watchdog) that runs from priority-0 deferred drains only (§9.1) |
| INV-MK-7 | Capabilities SHALL be non-amplifying. Revoke SHALL invalidate derivations only; deleting one's own cap SHALL NOT invalidate other holders. | `cap/cap.cpp` | VIOLATED (F2): `cap::revoke` clears the slot and calls the object's `revoke()` (object-wide); no `CAP_DELETE` exists |
| INV-MK-8 | Bulk channels SHALL be single-producer / single-consumer: `head` written only by the producer, `tail` only by the consumer. | `ipc/shm_ring.hpp` | TRUE; ring header lacks an epoch (§5.4) |
| INV-MK-9 | An absent mechanism SHALL deny, never degrade silently: no IOMMU domain ⇒ no DMA cap; stale IRQ slot ⇒ refuse; missing cap ⇒ typed error. | `syscall_handlers_iommu.cpp`, `syscall_handlers_irq.cpp` | TRUE for IOMMU/IRQ arm paths |
| INV-MK-10 | Every mechanism SHALL exist on x86_64, aarch64 and riscv64, or carry a documented arch-equivalent and a tracked issue (§11). | per-arch matrix | PARTIAL (§11) |
| INV-MK-11 | A server death SHALL wake or error-reply every client blocked on it, including reply waiters. | death funnel + IPC teardown | VIOLATED (F7): `MessageQueue::~MessageQueue` wakes queue-full senders only (§9.2) |
| INV-MK-12 | Priority boosts and donations SHALL be revertible and symmetric, mutated only through scheduler helpers, and bounded in chain depth. | `Scheduler::move_priority` | PARTIAL: single-level boost in `IPC::block_sender` only; not transitive (§8.1) |

## 2. Kernel mechanism boundary (current vs end state)

| Subsystem | Now (verified) | End state | Issue |
|---|---|---|---|
| Scheduler | O(1) bitmap, RMS + sporadic budget, EDF peek, affinity; no scheduling-context object (no `SchedContext` symbol in tree) | keep core; add SchedContext object, bind/unbind/donate, passive servers, timeout faults | #190 |
| IPC | per-task `MessageQueue`; `send` / `recv` (optional arg3 timeout, 0 = forever) / `send_sync`; FAST register path (48 B); SHM SPSC rings; Notify | keep core; add reply object + `REPLY_RECV`, direct switch on Call/ReplyRecv, death wake of reply waiters | #190, F3, F7 |
| Memory | PMM + `MemPool` (hard-coded table `memory/mempool.cpp:MemPool::init`) + `BufferPool`; Untyped carve (`CAP_RETYPE`, single fixed target); `FRAME_CREATE` allocates from PMM | kernel allocation-free after handover; frame/region manager PD owns Untyped; creation by retype only | #193 |
| IRQ | `IrqDelivery` slots; modes WAIT / NOTIFY / WAIT_ACK; MSI-X; storm limiter on aarch64 + riscv64 only | kernel routes direct to the owning PD; irq-mux owns configuration, sharing and IRTE only | #192, F8 |
| Time | 80–83 live in-kernel (`sys_clock_gettime`, `sys_nanosleep`, `sys_timer_create`, `sys_timerfd_create`); optional proxy `time/timer_srv.cpp` behind `CONFIG_TIMER_SRV` (default 0) | kernel keeps tick + replenishment + timeout faults; POSIX time is timer-srv RPC | #194 |
| VFS / FS | `vfs.cpp` core + 7 backends + fd table; vfs-srv shadows the kernel path (`vfs/vfs_srv.cpp`) | capability-mediated split with per-open channels; backends in PD | #517 (supersedes #47) |
| Block | in-kernel AHCI / ATA / virtio; block-srv live path (`driver/block_srv.cpp`) | block PD owns device via SDF caps + IOMMU domain | #517 |
| Drivers | PD sources `userspace/{serial,kbd,rtc,fb,block,net}-pd.c`; kernel probing retained | PDs own devices; kernel keeps routing slots only | #476 |
| Console / FB | `fb-pd.c` console PD over SHM; in-kernel `Terminal` and shell remain | emulation in PD; kernel keeps panic raw write only | #476 |
| Supervision | in-kernel `daemon_mgr` (`MAX_RESTART_COUNT = 10`, no backoff, polled from `on_tick`); death notify syscalls 66–68 | supervisor PD with restart policy, cap re-derive, client rebind | #191, #516 |
| Caps | `CapType` {Null, Task, Endpoint, Frame, CNode, Untyped, Mmio, Irq, IoMmuDma, Msix, Wdog} | add TCB, VSpace, PageTable, Notification, SchedContext; split delete/revoke | #50, F1, F2 |
| Process mgmt | `FORK/EXEC/WAITPID/KILL/GETPID` in kernel | proc-mgr PD over TCB/VSpace mechanisms | F11 |
| Syscalls | `MAX_SYSCALL = 97` (`syscall/syscall.hpp:SyscallNumber`) | closed certified set (§3) | #49 |

## 3. Target kernel API

### 3.1 Rule

The certified kernel API is a **closed set** of numbered entries. The set is
defined by Table T-1, not by a count in prose. `syscall_surface` SHALL assert
the exact set. Any addition requires SIL 3 justification (feeds #80). Retired
numbers SHALL NOT be reused.

Legend: **KEEP** certified mechanism · **KEEP-D** `CONFIG_DEBUG`-only,
excluded from the certified set · **MOVE** behaviour moves to a server RPC,
number retired · **RETIRE** number retired without replacement ·
**GATED** live, retirement gated on a migration step.

### 3.2 Table T-1 — disposition of every number 0–96

| Nr | Name | Today | Disposition |
|---|---|---|---|
| 0 | YIELD | live | KEEP |
| 1 | SEND | live | KEEP |
| 2 | RECEIVE | live; `arg3` = timeout ticks, 0 = forever (`syscall_handlers_ipc.cpp:sys_receive`) | KEEP |
| 3 | SEND_SYNC | live; Call semantics (`ipc.cpp:IPC::send_sync`) | KEEP |
| 4 | PRINT | live | RETIRE → log-srv RPC; panic raw write stays (not a syscall) |
| 5 | GET_TICKS | live | KEEP |
| 6 | EXIT | live, self only | KEEP |
| 7 | CREATE_MAILBOX | no-op stub returning success | RETIRE |
| 8 | DESTROY_MAILBOX | no-op stub | RETIRE |
| 9–19 | OPEN, READ, CLOSE, FSTAT, WRITE, LSEEK, IOCTL, READDIR, STAT, DUP, CHDIR | live | MOVE → vfs-srv |
| 20 | EXEC | live | MOVE → proc-mgr |
| 21 | FORK | live | MOVE → proc-mgr |
| 22 | WAITPID | live | MOVE → proc-mgr |
| 23 | GETPID | live | MOVE → proc-mgr |
| 24 | KILL | POSIX signal delivery | MOVE → proc-mgr; stop authority via N2 |
| 25 | PIPE | live | MOVE → vfs-srv (SHM-backed) |
| 26 | DUP2 | live | MOVE → vfs-srv |
| 27 | NOTIFY | live | KEEP |
| 28 | NOTIFY_WAIT | live | KEEP |
| 29 | EVENT_SET | live | MOVE → sync-srv |
| 30 | EVENT_WAIT | live | MOVE → sync-srv |
| 31 | SIGNAL | live | MOVE → proc-mgr (#164) |
| 32 | SIGRETURN | live | MOVE → proc-mgr (#164) |
| 33 | ALARM | live | MOVE → timer-srv |
| 34 | GETTOD | live | MOVE → timer-srv |
| 35 | UNAME | live | MOVE → supervisor RPC (static data) |
| 36 | PAUSE | live, FAST member | RETIRE; replaced by NOTIFY_WAIT or bounded RECEIVE (libc check F20) |
| 37–40 | BUF_ALLOC, BUF_FREE, BUF_MAP, BUF_UNMAP | live | MOVE → SHM-manager |
| 41–43 | MKDIR, UNLINK, RMDIR | live | MOVE → vfs-srv |
| 44–46 | BRK, GETRLIMIT, SETRLIMIT | live | MOVE → mem-mgr |
| 47 | GETRANDOM | live | MOVE → entropy-srv |
| 48 | KLOG | live | MOVE → log-srv |
| 49 | REBOOT | live | KEEP, supervisor-cap-gated |
| 50 | HALT | live | KEEP, supervisor-cap-gated |
| 51 | CAP_GRANT | live | KEEP |
| 52 | CAP_COPY | live | KEEP |
| 53 | CAP_REVOKE | live, object-wide | KEEP, semantics redefined: derivations only (INV-MK-7) |
| 54 | CAP_MINT | live | KEEP |
| 55 | IOPORT_GRANT | live, x86_64 only | KEEP; aarch64/riscv64: not applicable, fail-closed |
| 56 | CAP_RETYPE | live; target fixed per Untyped (`cap/untyped.hpp:retype_target`) | KEEP, extended: target type per invocation (F1) |
| 57 | IRQ_REGISTER | live; mode 0 WAIT, 1 NOTIFY, 2 WAIT_ACK | KEEP |
| 58 | IRQ_WAIT | live; arg1 timeout ticks, 0 = forever (`syscall_handlers_irq.cpp:sys_irq_wait`) | KEEP |
| 59 | IOMMU_MAP | live, x86_64 VT-d only | KEEP; other arches fail-closed (§11) |
| 60 | IOMMU_UNMAP | live, x86_64 only | KEEP |
| 61 | MMIO_MAP | live | KEEP (mechanism; policy in mem-mgr) |
| 62 | MMIO_UNMAP | live | KEEP |
| 63 | FRAME_CREATE | live; allocates from PMM | GATED → RETIRE at M12 |
| 64 | FRAME_MAP | live | KEEP |
| 65 | FRAME_UNMAP | live | KEEP |
| 66 | DEATH_WATCH | live | KEEP |
| 67 | DEATH_RECV | live | KEEP |
| 68 | DEATH_UNWATCH | live | KEEP |
| 69–73 | PAGER_REGISTER, PAGER_RECV, PAGER_MAP, PAGER_ABORT, PAGER_UNREGISTER | live | KEEP |
| 74 | SEND_FAST | live, register passing | KEEP |
| 75 | RECV_FAST | live; arg3 timeout ticks, 0 = forever (`syscall_handlers_ipc.cpp:sys_recv_fast`) | KEEP |
| 76 | SEND_SYNC_FAST | live; calls blocking `IPC::send_sync` | KEEP |
| 77 | SET_AFFINITY | live (`Scheduler::set_affinity_err`) | KEEP mechanism; placement policy → sched-policy srv |
| 78 | GET_AFFINITY | live | KEEP |
| 79 | TIMES | live (`Scheduler::read_times`) | KEEP |
| 80–83 | CLOCK_GETTIME, NANOSLEEP, TIMER_CREATE, TIMERFD_CREATE | live in-kernel | MOVE → timer-srv |
| 84 | ABI_VERSION | live | KEEP |
| 85 | TLS_SET | live | KEEP |
| 86–90 | TASK_DEBUG_ATTACH / READ_REGS / WRITE_REGS / READ_MEM / WRITE_MEM | live under `CONFIG_DEBUG`, ENOSYS in release | KEEP-D |
| 91 | WATCHDOG_CREATE | live, WdogCap-gated | KEEP |
| 92 | WATCHDOG_KICK | live | KEEP |
| 93–96 | THREAD_CREATE, THREAD_EXIT, THREAD_JOIN, THREAD_WRAP | live (#52, #53) | KEEP |

### 3.3 New entries (numbers `TBD at API freeze`)

| ID | Name | Justification |
|---|---|---|
| N1 | TCB_CONFIGURE | priority, affinity, SchedContext binding and fault endpoint as one cap-gated invocation |
| N2 | TCB_SUSPEND | supervisor must stop a hung PD; no mechanism exists (KILL is signal delivery) |
| N3 | TCB_RESUME | counterpart of N2; also starts a retyped TCB |
| N4 | VSPACE_MAP | map a PageTable object into a VSpace; proc-mgr and mem-mgr cannot build address spaces without it |
| N5 | REPLY_RECV | reply to the current caller and receive the next request in one entry; today a reply is `SEND` + sender-id filter (`ipc.cpp:IPC::send_sync`) |
| N6 | CAP_DELETE | delete one's own slot without invalidating other holders |
| N7 | SC_BIND | bind a SchedContext to a TCB |
| N8 | SC_UNBIND | counterpart of N7 |
| N9 | SC_DONATE | explicit donation where Call-implicit donation does not apply (if Q1 selects donation) |

FAST-tier membership of N1–N9 is decided at API freeze under the
pointer-free rule of `syscall-fastpath.md` §0.

### 3.4 Derived counts (informative; the set in T-1 is normative)

KEEP: 0, 1, 2, 3, 5, 6, 27, 28 (8), 49, 50 (2), 51–62 (12), 64–79 (16),
84, 85 (2), 91–96 (6) = 46.
New: N1–N9 = 9. Certified total = 55. KEEP-D: 86–90 = 5 (debug builds only).
MOVE/RETIRE/GATED: 4 (1), 7–26 (20), 29–48 (20), 63 (1), 80–83 (4) = 46.
46 + 5 + 46 = 97 =
`MAX_SYSCALL`, so every existing number has exactly one disposition.

### 3.5 Affinity and accounting split

The mechanism stays in the kernel: `Scheduler::set_affinity_err` (admission
probe), `Scheduler::queue_target`, `Scheduler::read_times`, and
`Scheduler::is_current_on_any_cpu`. Pinning per #167 is honoured: isolated RT
cores are excluded from the balancer. Placement policy moves to the
scheduler-policy server (F15). Violations fail with a typed error.

## 4. Kernel object model

### 4.1 Object types

Existing `CapType` (`cap/cap_types.hpp`): Null, Task, Endpoint, Frame, CNode,
Untyped, Mmio, Irq, IoMmuDma, Msix, Wdog.
Required additions (F1): TCB (split from Task as the schedulable object),
VSpace, PageTable, Notification, SchedContext, Reply (if Q2 selects reply
objects).

### 4.2 Retype

- Today an Untyped carries one fixed `retype_target` (default Frame) checked
  at retype (`cap/untyped.cpp`). The caller cannot choose the type.
- REQUIRED: the target type is a per-invocation argument of `CAP_RETYPE`. The
  Untyped records its physical range and watermark only.
- Every object type SHALL have a fixed, compile-time size; retype SHALL fail
  with a typed error if the remaining range is too small. No partial object.
- Retyped kernel objects SHALL be reclaimable only by revoking the parent
  Untyped's derivations, which destroys every object carved from it.

### 4.3 Delete vs revoke

- `CAP_DELETE` (N6): removes the caller's slot. If it was the last reference
  the object is destroyed; otherwise other holders are unaffected.
- `CAP_REVOKE` (53): destroys all derivations of the named cap; the named
  cap itself survives. Requires a derivation tree per object (#50).
- Both SHALL be bounded: revocation depth is bounded by `TBD at API freeze`;
  a revoke that exceeds the bound returns a typed error and is resumable.

### 4.4 Receive with timeout

`RECEIVE` (2) and `RECV_FAST` (75) already accept a timeout in ticks
(`syscall_handlers_ipc.cpp`). No new timeout-receive entry is required.

## 5. IPC contracts

### 5.1 Call

`SEND_SYNC` (3) and `SEND_SYNC_FAST` (76) are Call: send, then wait for a
reply from that receiver. Today the caller waits in a halt loop inside the
syscall (`ipc.cpp:IPC::send_sync`); the wait ends on reply, on bounded
timeout (`send_sync_bounded`), or never. REQUIRED: every Call on a server path
is bounded (INV-MK-3), and the death of the receiver wakes the caller with a
typed error (INV-MK-11).

### 5.2 Reply matching

Today replies are matched by sender id. `ipc.md` §2 accepts same-sender stale
replies as a residual. REQUIRED (Q2 DECIDED, owner, #512): each Call carries a
one-shot generation-tagged reply id (kernel-held token, Q1-style tag extended
to the call); a reply without a live token is dropped, and a consumed token
never matches again. The kernel stamps the caller identity on the Call request
path (plain `send_sync` included, mirroring `send_via_cap`), so token plus
stamped identity is unforgeable by a local peer. R-INPUT (owner, #512): every
server RPC validates its input first and rejects malformed messages with a
typed error; a service SHALL NEVER terminate because of a communication error.
No delegation: reply rights are not transferable in v0.6.1 (revisit only if a
protocol ever requires handoff; reply objects stay deferred with F1/F2).

### 5.3 FAST tier

`CONFIG_IPC_FAST_PAYLOAD_BYTES = 48`, pointer-free, oversize fail-closed
(`syscall-fastpath.md`, `ipc-fastpath.md`). FAST is register passing only. It
is not a direct switch and does not bypass the scheduler. Every control
message between servers SHALL fit 48 B (`BlockRpc` is `static_assert`ed
`== 48`, `TimerRpc` and `VfsRpc` `<= 48`); bulk data SHALL use §5.4.

### 5.4 Bulk channels

- SPSC rings in capability-mapped shared frames (`shm.md`,
  `ipc/shm_ring.hpp`), INV-MK-8.
- REQUIRED: the ring header gains an `epoch` field (header version bump). A
  peer SHALL reject a ring whose epoch differs from the epoch it bound to,
  and rebind (§10 D5).
- Wake-ups for rings use Notify, never polling loops without a bound.

### 5.5 Donation (if Q1 selects donation)

- Donated: the caller's SchedContext (budget, period, priority).
- Lifetime: lent on Call, returned exactly once on reply, on timeout fault,
  or on server death. A SchedContext SHALL NOT be held by two TCBs at once.
- Chain depth bound: `TBD at API freeze`; a Call that would exceed it fails
  with a typed error before blocking.
- Overrun: budget exhaustion inside a passive server raises a timeout fault
  to the server's fault endpoint; the handler resets the server to its
  receive state and error-replies the client.
- Passive server initialisation: a passive server SHALL be started on a
  temporary SchedContext, reach its receive point, and then unbind (N8). The
  same sequence is mandatory after every restart (§9.3).
- The external-pager inversion acceptance (`external-pager.md` B9) SHALL be
  replaced by this rule or by the Q1 alternative (F18).

If Q1 selects "no donation", the alternative is normative: every server's
priority SHALL be at least the highest priority of any client that calls it,
checked by `sdf_compose`; a composition that violates it fails.
R-TIME (owner, #512): Q1 is DECIDED for transitive donation (depth bound 4);
time exhaustion is PREVENTED by donation, not merely detected — every server
call runs under a donated context. Until M13 lands donation, the
enforced-priority rule above applies to every SDF row.

### 5.6 Latency model

- Mechanism: a blocking hop is applied at the next timer interrupt
  (INV-MK-5). At `CONFIG_TICK_HZ = 1000` each hop costs up to 1 ms. A file read
  routed client → vfs-srv → block-srv → vfs-srv → client has four blocking
  hops.
- REQUIRED: direct switch on Call and REPLY_RECV (migration M3, Q3 DECIDED scope):
  when the receiver is blocked in receive and runnable on this core, the kernel
  switches to it inside the syscall, without waiting for a tick. No
  synchronous-SEND switch (deferred to a follow-up only with new evidence).
- Per-hop bound and end-to-end overhead target: values frozen post-M0 against
  the F17 baseline (Q3 numeric half open; TCG-relative only until HW numbers
  exist).

## 6. Preemption, IRQ and lock discipline (#545, #546)

### 6.1 Facts

- x86_64 syscall entry is `int 0x80` through an interrupt gate (`0xEE`,
  `arch/x86_64/hal/idt.cpp:IDT::init`), so IF=0 on entry.
- `sync::SpinLock` is not priority-inheriting and publishes `locked_` before
  `holder_` (`sync/spinlock.hpp`); #545 observed a waiter spinning on
  `(locked=1, holder=NULL)`.
- The lock order `scheduler_lock_ → endpoint/queue lock` used by
  `IPC::block_sender` and the leaf locks `DeathNotify::s_lock_`,
  `g_iopb_lock`, `MessageQueue::lock_` are documented but not enforced.

### 6.2 Contract

1. Syscall handlers run with IF=0 until they block or return; a handler that
   re-enables interrupts SHALL do so only through `IrqGuard` scoping.
2. No spinlock SHALL be held at `reschedule()` or at any BLOCKED transition.
   DEBUG builds SHALL assert this at every `reschedule()` entry.
3. A task SHALL publish its wait channel before it becomes BLOCKED
   (reference pattern: `syscall_handlers_ipc.cpp:sys_receive`).
4. A task holding a non-PI spinlock SHALL NOT block or be descheduled
   voluntarily. Locks shared between task context and ISRs SHALL be taken
   with IRQs disabled.
5. Every IRQ-off region SHALL have a WCET bound recorded in the WCET table.
6. A tick watchdog in the TSC domain SHALL detect a stalled tick (#546).
7. Lock order (outer → inner): `scheduler_lock_` → endpoint / queue lock →
   leaf locks. DEBUG builds SHALL record and check acquisition order.

## 7. Static architecture, resources and budgets

### 7.1 System description

The SDF already exists: `task/boot_sdf.hpp` with `sdf_row` / `sdf_compose`
(all-or-nothing with rollback) and cap kinds IRQ, MSIX, MMIO, DMA, FRAME, SHM,
DMA_REGION. Roles: SERIAL, KBD, RTC, FB, BLOCK, NET, VFS_SRV, BLOCK_SRV,
TERM_SRV, LOG_SRV, CAP_BROKER, WDOG_MON, PROCFS_SRV (13).
REQUIRED additions: rows for supervisor, mem-mgr, irq-mux, timer-srv,
proc-mgr, sync-srv, entropy-srv, SHM-manager, sched-policy; per row a
priority, a budget/period, an Untyped quota, its channels, and its restart
class (§9.3). `sdf_compose` SHALL reject a description whose demand exceeds
any bound in §7.3. Each row additionally carries a core-affinity mask
(Q11-A: placement hint for future #167 pinning only; no cross-core
donation semantics in v0.6.1).

### 7.2 Fixed kernel pools

After handover the kernel keeps only fixed pools sized at build time: MemPool
classes (hard-coded in `memory/mempool.cpp:MemPool::init`: 16…8192 B, last
class 16384 B on aarch64), the IRQ slot table, the death-watch table, IOMMU
tables, IOPB pool. `sizeof(TaskControlBlock)` SHALL be checked by
`static_assert` against the largest MemPool class. The dead
`CONFIG_MEMPOOL_*` macros in `nexios_config.h` SHALL be removed or wired to
the table (F21). `CONFIG_STATIC_POOLS_ONLY` (default 0) is the certification
switch. Today it asserts only in `PMM::alloc_page`, `PMM::alloc_contiguous`
and `PMM::alloc_page_colored`; `PMM::alloc_user_page`,
`PMM::alloc_user_contiguous` and `PMM::alloc_page_table` are not gated, so
the switch does not yet forbid PMM allocation after init. REQUIRED (F21,
before M4's gate): every post-init PMM entry point SHALL be covered.

### 7.3 Bounds vs demand

| Bound | Value (`nexios_config.h`) | Demand / conflict |
|---|---|---|
| `CONFIG_MAX_TASKS` | 64 | all PDs + clients |
| `CONFIG_MAX_CPUS` | 8 | — |
| `CONFIG_CSLOT_COUNT` | 64 per CNode | SDF caps per PD + derived caps |
| `CONFIG_MAX_PER_TASK_OBJECTS` | 32 | mem-mgr owning many objects |
| `CONFIG_CAP_MAX_UNTYPED` | 16 live system-wide | CONFLICT: mem-mgr owning all free memory as Untyped |
| `CONFIG_CAP_MAX_DEATH_WATCHES` | 16 | supervisor watching every PD (≥ 13 roles + new rows) |
| `CONFIG_IOMMU_MAX_DOMAINS` / `_MAPPINGS` | 8 / 32 per domain | net RX rings, block DMA |
| `CONFIG_IOPB_MAX_TASKS` | 4 (x86_64) | CONFLICT: serial, kbd, rtc, ATA block all need port caps |
| `CONFIG_MAX_DAEMONS` | 16 | interim `daemon_mgr` |
| `CONFIG_IPC_FAST_PAYLOAD_BYTES` | 48 | all control RPCs |
| `CONFIG_ZOMBIE_STARVATION_LIMIT` | 32 | death latency (§9.1) |
| `CONFIG_IRQ_STORM_LIMIT` | 256 (aarch64, riscv64 users) | x86 has no limiter (§8.3) |

New values for conflicting bounds are an owner decision (Q7, F9).

### 7.4 WCET inputs

`scan_deadlines` worst case 2,030,000 cycles with 30 overrun tasks, measured
under TCG (`wcet-reaudit-v0.4.5.md` §3). It runs under `scheduler_lock_` in
the timer ISR. The composed system's task count SHALL be re-measured at each
gate; hardware measurement is required before certification.

## 8. Components

Each subsection gives: (1) current vs end state and syscall mapping,
(2) invariants and safety requirements, (3) protocols and IPC contracts,
(4) failure and supervision, (5) verification.

### 8.1 Scheduler and scheduling-context donation (#190)

1. **State.** No SchedContext object exists. Budgets live in the sporadic
   server (`task/sporadic_server.hpp`); an exhausted server drops to
   `bg_priority` (0 for vfsd and iocd, `taskdefs.cpp`). The only priority
   boost is in `IPC::block_sender` on a full queue. Syscalls: keeps 0, 77–79;
   adds N1, N7–N9.
2. **Invariants.** SchedContexts are fixed-size retyped objects (INV-MK-1).
   A donated SchedContext is returned exactly once (INV-MK-12). Boost or
   donation SHALL propagate along a blocked chain up to the depth bound, or
   the Call fails. The starvation breaker excludes level 0
   (`ready_queue_manager.cpp`); REQUIRED: an exhausted server SHALL NOT be
   demoted to level 0 unless the SDF declares it background-only, or the
   breaker SHALL be able to serve it.
3. **Protocol.** §5.5. Budget replenishment bounded by an `extra_refills`
   style limit per SchedContext, value `TBD at API freeze`.
4. **Failure.** Timeout fault → server fault endpoint; server death →
   donation returned to the client with a typed error (§10 D1).
5. **Verification.** `cap_schedctx`: donate/return exactly once; overrun
   fault; three-hop chain keeps the client's priority at every hop; server
   killed mid-call returns the donation; ResourceTracker delta 0.

### 8.2 Memory manager and frame/region manager (#193)

1. **State.** Kernel allocates from PMM, MemPool and BufferPool on client
   behalf (INV-MK-1 violations). `FRAME_CREATE` (63) allocates; `CAP_RETYPE`
   (56) carves Frames only. `PMM::alloc_user_page` performs no budget check
   or attribution, while `alloc_page` and `alloc_page_colored` do. End state:
   mem-mgr PD owns all free memory as Untyped; the kernel creates objects only
   by retype. Syscalls: keeps 56, 61, 62, 64, 65; adds N4; retires 63 at M12;
   moves 44–46 to mem-mgr RPC.
2. **Invariants.** Split into **bootstrap** (M4: root task receives all free
   memory as Untyped, kernel pools frozen) and **policy** (M12: retype-only
   creation). After M4 the BufferPool PMM fallback is removed; exhaustion
   returns a typed error. Every allocation path checks the per-task budget.
   R-MEM (owner, #512, mechanism #575): exhaustion/pressure past the
   threshold latches an explicit unstable-state warning (diagnostic +
   supervisor notify, zero-alloc), never silent coping — the supervisor
   answers by driving the §9.3 fail-safe rows.
3. **Protocol.** Clients request memory by RPC (≤ 48 B); mem-mgr retypes from
   its Untyped and grants the resulting cap. The kernel SHALL NOT need memory
   from mem-mgr to deliver mem-mgr's own IPC: every IPC object used by
   mem-mgr is allocated before handover.
4. **Failure.** mem-mgr is a critical server (§9.3): its death is a system
   fault, policy Q12. Revoking an Untyped destroys all objects carved from it;
   in-flight DMA to those frames is handled by §10 D2.
5. **Verification.** `mm_usermgr`: client RPC leaves PMM free count unchanged
   after M4; OOM returns a typed error, never a panic; partition isolation;
   `CONFIG_STATIC_POOLS_ONLY=1` build passes the class once the switch covers
   every post-init PMM entry point (§7.2, F21).

### 8.3 IRQ routing and multiplexer (#192)

1. **State.** `IrqDelivery` slots with WAIT / NOTIFY / WAIT_ACK. The storm
   limiter exists on aarch64 and riscv64 only. On x86 every PIC-kind arm
   programs a level-triggered IOAPIC entry (`irq_delivery.cpp`), and
   `mask_on_delivery` runs in WAIT_ACK mode only, so a WAIT-mode level line
   is not masked on delivery and x86 has no storm limiter behind it. On
   aarch64 the GIC line is masked on delivery in WAIT_ACK mode only; on
   riscv64 the PLIC line is deliberately never masked (edge semantics,
   `irq_delivery.cpp:mask_on_delivery`). `pending` is an unsaturated
   `uint32_t` counter; NOTIFY keeps only the last vector. `sys_irq_wait`
   accepts only the slot's recipient (`r->recipient != t` refused under the
   slot lock), so a second distinct waiter cannot register today; the
   single-waiter rule below is a hardening assertion, not a defect fix.
   Syscalls: keeps 57–60.
2. **Invariants.** Delivery goes from the kernel directly to the owning PD's
   waiter or Notify; the mux SHALL NOT be in the delivery path. Level lines
   SHALL be masked on delivery and unmasked only by the owner's ack. Per-line
   storm limiting SHALL exist on all three arches. `pending` SHALL saturate
   and count overflow. A second waiter on a slot SHALL fail with a typed error.
3. **Protocol.** irq-mux holds IrqCaps for shared or remappable lines and
   grants per-line or per-MSI-X-entry caps to drivers; IRTE programming is a
   mux request to the kernel. NOTIFY delivery SHALL carry a per-line pending
   bitmask, not a single last vector.
4. **Failure.** Owner death masks the line, drains `pending`, clears the
   waiter, and bumps the slot generation (§10 D3). Mux death leaves existing
   routes live; only reconfiguration is unavailable.
5. **Verification.** `irq_mux`: owner-only routing; storm on a stuck level
   line is bounded on each arch; revoke and death drain; MSI-X entry
   isolation; double-waiter refusal.

### 8.4 Timer service and timekeeping (#194)

1. **State.** 80–83 are live in-kernel; `time/timer_srv.cpp` is an optional
   proxy (`CONFIG_TIMER_SRV`, default 0) with a 16-entry tombstone ring
   (`timer-srv.md`). End state: kernel keeps tick, budget replenishment and
   timeout faults; timer-srv multiplexes all POSIX time. Syscalls: moves
   33, 34, 80–83; keeps 5.
2. **Invariants.** timer-srv state is fixed-size; the client table size is an
   SDF parameter. Sleeping clients block in bounded `RECEIVE` or
   `NOTIFY_WAIT`, never in a kernel timer wheel after the flip.
3. **Protocol.** 48 B `TimerRpc`; replies validated on server pid, epoch,
   owner id + generation, timer id + slot generation (`timer-srv.md`).
   timer-srv's own wake source is a tick Notify from the kernel.
4. **Failure.** Epoch bump on restart; clients re-arm. Tombstone overflow
   SHALL be counted and reported, not silently dropped (§10 D6).
5. **Verification.** `srv_timer`: no kernel blocking for sleep after flip;
   50-timer multiplex; restart re-arm; drift bound against the tick.

### 8.5 VFS backends and block storage server (#517)

1. **State.** Kernel VFS is authoritative; vfs-srv shadows it with seven
   backends (`vfs-srv.md`); block-srv has a live path with IRQ delegation
   (`block-srv.md`). #47 is closed; remaining scope is #517. Syscalls: moves
   9–19, 25, 26, 41–43.
2. **Invariants.** Per-open channels are capabilities; fd tables live in
   vfs-srv. Authorization is done once at the boundary. Block DMA only into
   frames mapped in the block PD's IOMMU domain (INV-MK-9).
3. **Protocol.** 48 B `VfsRpc` / `BlockRpc` control; data through SPSC rings
   per open channel; every RPC bounded (INV-MK-3).
4. **Failure.** vfs-srv death: clients receive typed errors on open channels
   and reopen after restart. block-srv death: device quiesced and IOMMU
   domain detached before frames are reclaimed (§10 D2).
5. **Verification.** `vfs_srv_live`, `block_srv_live` parity (byte-identical
   to the kernel path); `vfs_server_restart`; `block_dma_isolated`;
   `vfs_authorize_toctou`. Shadow-mode flip criterion: §12.

### 8.6 Drivers and console / framebuffer PDs (#476; #48, #51 closed)

1. **State.** PDs exist for serial, kbd, rtc, fb, block, net. The console PD
   renders over SHM (4 ring pages + 1 status page, scanout in 2 MiB chunks,
   `boot_sdf.hpp`). The in-kernel `Terminal` and shell remain. Syscalls: keeps
   55, 57–62; retires 4 and 48 into log-srv.
2. **Invariants.** One owner per device; no ambient authority; restart
   re-derives caps via `sdf_compose` (`driver-pds.md`). Render work per pass
   is bounded. Port-using PDs fit `CONFIG_IOPB_MAX_TASKS` (§7.3 conflict).
3. **Protocol.** Console text over SPSC ring; status page counters; kbd input
   via ring + Notify.
4. **Failure.** fb-pd death: kernel panic path keeps raw write; console
   output buffers in the ring up to its capacity, then drops with a counter.
5. **Verification.** `fb_bounded_render`, `driver_crash_isolated`,
   `irq_no_ambient`, `fb_fault_isolated`, `scroll_bounded`, `log_no_block`.

### 8.7 Supervisor and death notification (#191, #516)

1. **State.** In-kernel `daemon_mgr` restarts up to 10 times without backoff,
   polled from `on_tick`. Death records latch only in
   `TaskControlBlock::cleanup()`; fault-path kills bypass `notify_death`
   (#516). Syscalls: keeps 49, 50, 66–68; adds N2, N3; receives 35.
2. **Invariants.** INV-MK-6 and INV-MK-11. The supervisor SHALL hold a
   death watch for every supervised PD (bound §7.3). Restart SHALL revoke the
   dead PD's derivations before re-derive (§10 D4).
3. **Protocol.** Death record → supervisor Notify → `DEATH_RECV`; restart =
   revoke derivations, retype fresh objects from the PD's Untyped quota,
   `sdf_compose` the row, bump the server epoch, publish the new endpoint.
   Clients rebind on epoch change. This full protocol needs F1/F2/F4 (M11)
   and the Untyped quota (M4, M12) and applies from M12. Interim (M5 to
   M12): dispose the dead PD's own slots, re-mint via `task/boot_sdf.hpp:
   sdf_rebind`, bump the epoch; no revoke of other holders' caps. #515
   (cap-broker policy serving) lands alongside, its kernel consult
   invalidated by the same epoch bump. Restart policy per SDF row: restart
   class, maximum restarts, backoff (values `TBD at API freeze`).
4. **Failure.** Supervisor death and mem-mgr death are critical (§9.3,
   Q12). Watch-table exhaustion fails the restart closed and is reported.
5. **Verification.** `srv_supervisor`: crash-restart loop with tracker delta
   0; `mk_death_visible_bounded` under full CPU load; fault-killed PD is
   reported (#516); boot composition grants exactly the SDF caps.

### 8.8 Process manager (F11)

1. **State.** No PD, no issue. 20–24 and 31–32 are in-kernel.
2. **Invariants.** Requires N1–N4 and the retype types of §4.1. `fork` is
   built from retype + VSpace map; no kernel deep copy after the flip.
3. **Protocol.** proc-mgr holds TCB caps for every task it created.
4. **Failure.** Critical-server class decided by Q12.
5. **Verification.** `proc_elf` parity; `pid_reuse_safe`.

### 8.9 Log server

1. **State.** `log::DmesgService` and `log::KlogService` singletons in the
   kernel; `log/log_srv.cpp` proxy exists.
2. **Invariants.** The kernel keeps a fixed ring and the panic raw path;
   log-srv is a sink and mirror. Early boot logs stay in the kernel ring until
   log-srv drains them.
3. **Protocol.** Ring + Notify; producers never block on log-srv.
4. **Failure.** log-srv death drops nothing from the kernel ring; drain
   resumes after restart.
5. **Verification.** `logging_debug`; `log_no_block` under log-srv kill.

### 8.10 Servers without code

proc-mgr (F11), sync-srv (F12), entropy-srv (F13), SHM-manager (F14),
scheduler-policy server (F15). Each SHALL get an SDF row, a restart class and
a test class before its MOVE rows in T-1 take effect.

## 9. Failure, supervision and death semantics

### 9.1 DEATH-1 — death record latency

`DeathNotify::on_task_death` runs only inside
`TaskControlBlock::complete_teardown()`, reached from
`TaskControlBlock::cleanup()` and `TaskControlBlock::destroy()`. Callers of
`cleanup()`, by death path:

- `Scheduler::terminate` deaths (kill, fault, watchdog; `scheduler_terminate.cpp`)
  call `Scheduler::release_zombie`, which removes the task from `all_tasks_`,
  so `Scheduler::reap_orphans` never reaches it. `cleanup()` then runs only
  from: idle `Scheduler::cleanup_step` (`memory/integrity.cpp`), VMM teardown
  `Scheduler::cleanup_step_try` (`memory/vmm_teardown.cpp`),
  `Scheduler::flush_zombies` when the zombie count exceeds
  `CONFIG_ZOMBIE_STARVATION_LIMIT` (checked every 100 ticks,
  `scheduler_tick.cpp`), and `Scheduler::drain_zombie_list`, driven by the
  PID 1 reaper loop (`kernel.cpp`, poked on every zombie birth but running at
  priority 0 outside the shell `selftest` run, which raises it to 10) and by
  `irq_thread.cpp` / `taskdefs.cpp` teardown helpers.
  Idle and the priority-0 reaper are both outside the starvation breaker,
  so under sustained load visibility is bounded only by the zombie-count
  trigger, not by time.
- Self-exit (`syscall_handlers_misc.cpp:sys_exit` →
  `Scheduler::switch_away_from_terminating`) does not call `release_zombie`;
  the task stays in `all_tasks_` and is torn down by the parent's
  `syscall_handlers_process.cpp:sys_waitpid` (synchronous `cleanup()`), or by
  `Scheduler::reap_orphans` → `note_termination_and_free` (every 100 ticks
  from `on_tick`, when `parent_id == 0`, when the parent is a RUNNING PID 1,
  or when the parent is TERMINATED or absent from the id table).
- `TaskControlBlock::destroy` covers creation and compose failures.
REQUIRED (F6): the death record is latched at the terminate funnel (state
transition to zombie) for every death cause including fault kills (#516);
resource teardown may stay deferred. Visibility bound (Q4 DECIDED, owner,
#512): synchronous latch; supervisor-observable ≤ 2 ticks given the
supervisor is runnable at highest priority.

### 9.2 DEATH-2 — reply waiters

`MessageQueue::~MessageQueue` wakes queue-full senders only. A client in
`reply_wait` on a dead server is not woken and waits until its own bound
(or forever on plain `send_sync`). REQUIRED (F7): on server death the kernel
wakes every reply waiter targeting that server with a typed error.

### 9.3 Restart classes

| Class | Members | Policy |
|---|---|---|
| critical | supervisor, mem-mgr | death is a system fault; two-phase recovery (Q12 DECIDED, owner, #512): drive the SDF-declared fail-safe state FIRST (R-FAILSAFE — safe outputs, quiesce), THEN reboot. Reboot-loop guard required (consecutive-early-death counter → fail-stop, mechanism TBD with watchdog work). Redundant supervisor is a post-v0.6 follow-up only if availability analysis demands it; never for mem-mgr |
| restartable | drivers, block-srv, vfs-srv, timer-srv, log-srv, term, procfs, irq-mux | supervisor restart per §8.7 |
| client | applications | reported to proc-mgr / supervisor; no restart by default |

A restarted passive server is initialised per §5.5 before it serves.

R-MEM (owner, #512): memory exhaustion is unrecoverable — the manager MUST emit an explicit unstable-state warning (diagnostic + supervisor notify, zero-alloc latched path, mechanism #575) instead of failing silently. The supervisor reacts to the warning by driving the fail-safe rows above. Every SDF row SHALL declare its fail-safe contribution (safe output values, quiesce order); `sdf_compose` rejects a description without one for restartable/critical members.

## 10. Reclaim and leak rules on death or restart

| Rule | Resource | Requirement |
|---|---|---|
| D1 | donated SchedContext | returned to the donor with a typed error before the server's objects are reclaimed |
| D2 | in-flight DMA | quiesce the device (mask its IRQ, bounded wait) and detach its IOMMU domain before any of its frames are reclaimed; no domain ⇒ no DMA cap (INV-MK-9) |
| D3 | IRQ slot | mask the line, drain `pending`, clear `waiter`, bump slot generation |
| D4 | capability derivations | revoke the dead PD's derivations (not other holders, INV-MK-7) before `sdf_compose` re-derives |
| D5 | SHM rings | epoch-tag; peers unmap and rebind on epoch mismatch |
| D6 | timer tombstones | ring overflow is counted and reported; capacity from SDF |
| D7 | BufferPool handles | released on owner death; no PMM fallback after M4 |
| D8 | death-watch slots | 32 static slots fail closed (Q7); a supervisor with more watched PDs SHALL be split or the bound raised |

Every rule is verified by a kill/restart cycle under `test_isolate` with
`ResourceTracker::check` delta 0.

## 11. Cross-architecture parity matrix

| Mechanism | x86_64 | aarch64 | riscv64 | Issue |
|---|---|---|---|---|
| Syscall entry IF state | interrupt gate, IF=0 | `svc` entry, IRQs masked | `ecall` entry, SIE cleared | #546 |
| IRQ controller | PIC / IOAPIC / MSI-X | GICv2 (v3 path) | PLIC | #192 |
| IRQ storm limiter | ABSENT | present | present | F8 |
| Line mask on delivery (`irq_delivery.cpp:mask_on_delivery`) | WAIT_ACK only; WAIT mode programs a level IOAPIC entry and is not masked | WAIT_ACK only (GIC mask); WAIT mode relies on the storm limiter | never masked (PLIC edge semantics); storm limiter only | F8 |
| Deferred-switch apply point (INV-MK-5) | `isr_stubs.asm` epilogue | `vectors.S` epilogue | `syscall_entry.S` / `interrupt_controller.cpp` trap epilogue | #546 |
| IOMMU | VT-d | SMMUv3 pending; `sys_iommu_map`/`unmap` dispatch tests fail instead of being arch-scoped (#528) | none: DMA PDs fail closed (Q10) | #168, #528 |
| I/O ports | IOPB, 4 tasks | not applicable, fail-closed; ioport dispatch tests fail instead of being arch-scoped (#528) | not applicable, fail-closed | F9, #528 |
| Debug syscalls 86–90 (KEEP-D) | green (`core_syscall` debug, `test-history.txt` 2026-11-13) | green (`core_syscall` debug, `test-history.txt` 2026-11-13) | `debug_launch_entry_trap_parks` flakes under load (#544) | #544 |
| SMP tests | `smp`, `smp_multicpu` | not run | not run | #525 |
| Console PD runtime | green | green | was blocked by #513 (closed); re-verify at next gate | #476 |
| Cross-core IPC / donation | out of scope unless Q11 decides otherwise | same | same | #167 |

## 12. Configuration and build modes

- Each server lands behind `CONFIG_<X>_SRV`, default 0, following
  `CONFIG_TIMER_SRV`.
- A certified build SHALL select exactly one path per subsystem (kernel or
  server) at compile time. Runtime failover from a crashed server back into
  kernel code SHALL NOT exist.
- The kernel fallback is kept for one milestone after a flip, as a build
  option only, then deleted after the subsystem's gate.
- vfs-srv shadow-mode exit criterion (Q6 DECIDED, owner, #512 — closed list, no
  further TBD): (a) byte-identical results across the parity class;
  (b) tracker delta 0; (c) zero fallback hits over the full gate run on all
  three arches; PLUS (d) fault-injection parity (corrupt image, full
  filesystem, I/O-error legs return identical errors, not just identical
  bytes on success); (e) restart-in-flight proven pre-flip
  (`vfs_server_restart`, `vfs_authorize_toctou` green, tracker delta 0 across
  the kill cycle); (f) performance ceiling (VFS op latency within the M3
  per-hop budget × chain depth, TCG-relative with headroom, absolute numbers
  HW-only); (g) soak + concurrency (3 consecutive full-gate greens incl. the
  `storage` class under concurrent multi-client load, not just sequential
  parity). Happy-path-only equivalence is explicitly insufficient.

## 13. Verification plan

### 13.1 Entry points

Only these are valid:
- `python3 tools/meson/qemu_test.py --arch <arch> --build <build> --class <class>`
- `bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} {debug,release}`
  (26 generic classes on x86_64; aarch64 and riscv64 append their per-arch
  extras from `tools/meson/list_tests.py`; the 2×3 matrix is the release
  gate, 0 failures, no arch exempt). The `all` class and `test-full` are not valid entry points.

### 13.2 Rules

- New tests start as `JARVIS_TEST_PASS()` stubs with `JARVIS_REGISTER_TEST`
  and a `test_expected_counts.hpp` update on `testbed`; real asserts follow.
- Every test runs under `test_isolate` snapshot/restore with
  `ResourceTracker::check` delta 0, including kill/restart cycles. Drain paths
  respect `Scheduler::is_current_on_any_cpu`.
- A test for a VIOLATED invariant is marked known-fail until its fix lands;
  it is never registered as green.

### 13.3 Planned tests

| Test | Asserts |
|---|---|
| `syscall_surface` | exact T-1 KEEP set + N1–N9; KEEP-D only under `CONFIG_DEBUG` |
| `mk_inv_no_kernel_alloc` | INV-MK-1 after M4 |
| `mk_inv_static_bounds` | INV-MK-2 demand ≤ bound for the composed SDF |
| `mk_rpc_bound` | INV-MK-3 typed error at each declared bound |
| `mk_death_visible_bounded` | INV-MK-6 under full CPU load |
| `mk_reply_waiter_wake` | INV-MK-11 |
| `mk_cap_delete_vs_revoke` | INV-MK-7 |
| `mk_retype_target_types` | §4.2 |
| `mk_supervisor_stop` | N2 / N3 |
| `mk_ring_epoch_reject` | D5 |
| `mk_irq_slot_death_drain` | D3 |
| `cap_schedctx`, `mm_usermgr`, `irq_mux`, `srv_supervisor`, `srv_timer` | §8.1–§8.4, §8.7 |
| `revoke_cascade`, `stale_use_fails`, `death_drain` | #50 |
| `core_footprint`, `no_kernel_fs` | #49 kernel minimality (#89 tracker) |
| `vfs_server_restart`, `block_dma_isolated`, `vfs_authorize_toctou` | §8.5 |
| `driver_crash_isolated`, `irq_no_ambient`, `fb_fault_isolated`, `scroll_bounded`, `log_no_block` | §8.6, §8.9 |
| `pid_reuse_safe` | §8.8 |
| `cap_broker_live` | #515 policy serving |

### 13.4 Existing classes per step

`core_sched`, `core_sync`, `ipc`, `capability`, `servers`, `cap_broker`,
`block_srv_live`, `vfs_srv_live`, `storage`, `drivers`, `ui`,
`logging_debug`, `deadline`, `proc_elf`, `smp` / `smp_multicpu` (x86_64
only, per `tools/test-policy.tsv`).

## 14. Migration sequence

Every step's gate: (a) the named classes green on all three arches, (b)
fault injection (kill, overrun, revoke) green, (c) SIL 3 audit APPROVED, (d)
tracker delta 0 on kill/restart. The 2×3 matrix runs before every flip and
before every kernel deletion.

| # | Step | Needs | Gate classes |
|---|---|---|---|
| M0 | IPC baseline: define the workload, record per-hop cost of the current path (F17) | — | `ipc`, `bench` |
| M1 | DEATH-1 and DEATH-2: latch at terminate funnel incl. fault kills; wake reply waiters (F6, F7, #516) | — | `capability`, `servers`, `ipc` |
| M2 | Preemption / lock contract: fix #545, add §6.2 assertions and tick watchdog (#546) | — | `core_sched`, `core_sync`, `ipc` |
| M3 | Direct switch on Call / REPLY_RECV; reply token (F3, #398, Q2, Q3) | M2 | `ipc`, `bench` |
| M4 | mem-mgr bootstrap: root task Untyped handover, frozen pools, BufferPool fallback removed, user-page budget (F10) | M3 | `core_memory`, `capability` |
| M5 | Supervisor skeleton: watches all PDs, interim restart (dispose own slots + `sdf_rebind`, §8.7 item 3), epoch publish; cap-broker policy serving (#515); `daemon_mgr` interim. Full revoke-then-retype restart and N2/N3 follow at M12 | M1 | `servers`, `cap_broker` |
| M6 | timer-srv flip | M3, M5 | `deadline` |
| M7 | log-srv flip | M3, M5 | `logging_debug` |
| M8 | IRQ hardening (x86 limiter, level mask, `pending` saturation, single waiter), then irq-mux config role (F8, #192) | M2 | `capability`, `drivers` |
| M9 | Driver PDs → block-srv → VFS (#476, #517) | M4, M8 | `drivers`, `storage`, `block_srv_live`, `vfs_srv_live` |
| M10 | Console / term and procfs | M9 | `ui`, `servers` |
| M11 | Object model: CapType additions, per-invocation retype, CAP_DELETE, TCB_*, VSPACE_MAP (F1, F2, F4, F5) | M3 | `capability`, `proc_elf` |
| M12 | mem-mgr policy: retype-only creation; retire FRAME_CREATE; supervisor switches to the full §8.7 item 3 restart protocol | M4, M5, M11 | `core_memory`, `capability`, `servers` |
| M13 | SchedContext donation or enforced priority rule (#190, Q1) | M3, M11 | `cap_schedctx`, `core_sched` |
| M14 | proc-mgr | M11, M12 | `proc_elf`, `core_task` |
| M15 | sync-srv, entropy-srv, SHM-manager, sched-policy | M11 | named classes |
| M16 | Per-subsystem kernel deletion after its gate, then 2×3 matrix | each flip | 2×3 matrix |

Rationale: mem-mgr is bootstrap-early and policy-late because every server
needs frames at its first RPC. Donation (M13) follows the object model; until
it lands the §5.5 enforced-priority rule applies to every SDF row.

## 15. Follow-up issues

| ID | Issue | Title | Scope |
|---|---|---|---|
| F1 | #554 | Object model: CapType additions + per-invocation retype target | `cap/cap_types.hpp`, `cap/untyped.*` |
| F2 | #555 | Split CAP_DELETE from CAP_REVOKE | `cap/cap.cpp` |
| F3 | #556 | Reply token + REPLY_RECV + direct switch on Call | `ipc/ipc.cpp`, ISR epilogue |
| F4 | #557 | TCB_CONFIGURE / TCB_SUSPEND / TCB_RESUME | new |
| F5 | #558 | VSPACE_MAP | new |
| F6 | #559 | Death record at terminate funnel, bounded visibility | extends #516 |
| F7 | #560 | Wake reply waiters on server death | `ipc/ipc.cpp` |
| F8 | #561 | x86_64 IRQ parity: storm limiter, level mask, `pending` saturation, single waiter | `irq_delivery.cpp`, `syscall_handlers_irq.cpp` |
| F9 | #562 | Static bound re-sizing (Untyped, per-task objects, CNode slots, IOPB, death watches) | `nexios_config.h` |
| F10 | #563 | mem-mgr bootstrap handover | #193 part a |
| F11 | #564 | proc-mgr PD | new |
| F12 | #565 | sync-srv PD | new |
| F13 | #566 | entropy-srv PD | new |
| F14 | #567 | SHM-manager PD + ring epoch | new |
| F15 | #568 | Scheduler-policy server | new |
| F16 | #569 | log-srv completion: PRINT/KLOG retirement | `log/log_srv.cpp` |
| F17 | #570 | IPC baseline and overhead harness | `bench` |
| F18 | #571 | Reconcile `external-pager.md` B9 with §5.5 | spec |
| F19 | done (file deleted) | Retire `docs/specs/microkernel-transition-udpate.md` (superseded; owner approval required to delete) | doc |
| F20 | #572 | libc dependency audit for PAUSE and 80–83 retirement | `src/libc/syscall.h` |
| F21 | #573 | Remove or wire dead `CONFIG_MEMPOOL_*`; TCB size `static_assert`; extend the `CONFIG_STATIC_POOLS_ONLY` assert to `alloc_user_page`, `alloc_user_contiguous`, `alloc_page_table` | `nexios_config.h`, `mempool.cpp`, `pmm.cpp` |

## 16. Owner decisions (Q1–Q12 DECIDED per #512)

| Q | Decision |
|---|---|
| Q1 | DECIDED (owner, #512): transitive SchedContext donation, depth bound 4; enforced SDF priority rule as M5–M12 interim gate (§5.5) |
| Q2 | DECIDED (owner, #512): generation-tagged reply id (kernel-held one-shot token); kernel stamps sender_id on the Call path; no-delegation note in §5.2; reply objects deferred until a protocol needs handoff |
| Q3 | DECIDED scope (owner, #512): direct switch on Call + REPLY_RECV only (no synchronous-SEND switch); per-hop and end-to-end numeric targets frozen post-M0 against the F17 baseline |
| Q4 | DECIDED (owner, #512): latch synchronous at terminate funnel (all death causes); supervisor-observable ≤ 2 ticks given supervisor runnable at highest priority (see §9.1) |
| Q5 | DECIDED mechanism (owner, #512): per-server SDF-declared RPC bounds, values set post-M0 (TCG-relative, ≥3× headroom); uniform 5000-tick guard stays as interim ceiling; per-operation-class refinement deferred to #574 (v1.1.0) |
| Q6 | DECIDED closed list (owner, #512): byte-identity + tracker-0 + zero fallback hits, PLUS fault-injection parity, restart-in-flight proven pre-flip, performance ceiling (M3 budget × depth), soak + concurrency (3 consecutive greens). See §12 |
| Q7 | DECIDED values (owner, #512; landed via F9/#562): CAP_MAX_UNTYPED 64; MAX_PER_TASK_OBJECTS 256 for mem-mgr/supervisor rows, 32 elsewhere; CSLOT_COUNT 64 per CNode with 2 CNodes for infra servers; CAP_MAX_DEATH_WATCHES 32; IOPB_MAX_TASKS 8 (x86_64); IOMMU 8 domains / 128 mappings per domain; MAX_TASKS 64, FAST_PAYLOAD 48, ZOMBIE_LIMIT 32, STORM_LIMIT 256 unchanged |
| Q8 | DECIDED (owner, #512): port the identical limiter to x86 (one `g_storm`, same 256/tick same-INTID rule, explicit timer-vector exemption named in the port); extend the stuck-line test to x86. Per-vector knob rejected (unmeasured need, wrong layer for MSI-X floods). Single-slot A/B-alternation blind spot noted as a follow-up candidate only (per-slot counters, filed if demonstrated) |
| Q9 | DECIDED subtree-only (owner, #512): N2/N3 supervisor-cap-gated + supervised-set membership check (SDF watches = authority, compose-checked); exclusions hardcoded (never self, never mem-mgr, never kernel tasks); proc-mgr owns client stop, supervisor owns server stop; cross-subtree attempts fail closed with a typed error. Multi-supervisor delegation is a post-v0.6 protocol |
| Q10 | DECIDED gated follow-up (owner, #512): #577 tracks RISC-V IOMMU (criteria-gated: QEMU virt emulation + stable programming interface + in-tree test gate; v0.8.0 family with #168/#525); interim riscv64 DMA PDs stay fail-closed. Permanent fail-closed REJECTED (two-tier drivers vs INV-MK-10) |
| Q11 | DECIDED single-core scope (owner, #512): IPC + donation semantics are single-core in v0.6.1; cross-core wake stays tick-deferred via mailbox/IPI; SDF carries a core-affinity field (needed by #167) but cross-core donation transfer is out of scope. Accepted as an explicit fidelity gap (unprovable on single-hart QEMU); follow-up #576 filed (depends on #525 SMP coverage) so the gap cannot fossilize |
| Q12 | DECIDED uniform two-phase (owner, #512): critical-server death (supervisor, mem-mgr) drives the system into an SDF-declared fail-safe state FIRST (R-FAILSAFE), THEN reboots (MINIX-RS/seL4-init precedent). Reboot-loop guard required (consecutive-early-death counter → fail-stop, mechanism TBD with watchdog work). Redundant supervisor filed as post-v0.6 follow-up only if availability analysis demands it; never for mem-mgr (revoke-cascade makes survival incoherent). Kernel recovery path REJECTED (violates INV-MK-1, reintroduces kernel omnipotence) |

## Appendix A — Implementation record (non-normative)

| Commit | Content |
|---|---|
| `d1b44d849` | #47 step-a vfs + block PDs: SDF, PDs, parity, fault demo (no flip) |
| `960b27673` | #51 step-c-prime serial SHM SPSC ring TX |
| `fc4a5e0e8` | #47 step-b1 block-srv live path |
| `ace5c4da1` | #47 step-b2 block-srv IRQ delegation (WAIT_ACK, bounded wait, poll fallback) |
| `579605bd4` | #47 step-b3 vfs-srv backends live path |
| `27842ffa2` | #51 console PD substrate (SDF FB row, fb-pd render, `fb_bounded_render`) |
