# Microkernel Transition — System Transformation Spec (Phase 8, v0.7.x–0.8.x)

**Status:** draft iter-3 (planner iter-2: count 34→38 fixed; `queue_target` verified scheduler.hpp:816) · **Milestone:** `[v0.7.x] Transition to Microkernel` (to be created — ROADMAP.md currently has no v0.7.x milestone; Phase 8 un-milestoned; step §7.0 creates it first)
**Issues:** #47–#54 (existing) + #190–#194 (gap closure, this cycle)
**Author:** developer (orchestrator) · **Reviewer:** planner subagent (Phase 1 pipeline)

## 0. Goal

Transform NexIOS from a monolithic kernel with in-kernel daemons into a real
microkernel: the kernel provides **threads + scheduling contexts, address
spaces, IPC (endpoints/notifications incl. fastpath), capability spaces with
take-grant/revoke, IRQ routing slots, and tick + budget enforcement** — and
nothing else. VFS/filesystems, block storage, drivers, console/framebuffer,
network, procfs, timers/POSIX time, paging policy, and process management move
to isolated userspace servers composed by a **static system architecture**
with a **supervisor** (restart/reincarnation).

Non-goals: rewriting the scheduler core (O(1) bitmap + RMS/EDF stays),
changing the x86_64 trap path, or touching Phase 9 protocol stacks (TCP/IP,
USB) beyond their server scaffolding.

## 1. Modern background (what "real microkernel" means in 2025–2026)

- **seL4 (~10 kLOC, verified):** capabilities as unforgeable tokens in CNodes,
  take-grant propagation, recursive revoke; 3 syscalls conceptually
  (Send/Recv/Yield + Call variants); no kernel allocation (all objects from
  user Untyped); IPC ~0.2 µs ARM64, +5–10% vs monolith at system level.
- **seL4 MCS:** scheduling contexts (budget/period) as first-class cap
  objects; **passive servers** execute on the **donated caller SC**
  (`Call` donates, reply returns); **timeout faults** to a handler endpoint
  on overrun; `extra_refills` bounds sporadic-server fragmentation.
  This is the principled fix for server priority-inversion/overrun —
  direct-process-switch donation without accounting is unanalysable.
- **LionsOS (seL4/Microkit):** static architecture (SDF: PDs, channels, memory
  regions, IRQ bindings generated into setup); strictly-sequential servers;
  **SPSC shared-memory** channels + semaphores between driver and clients;
  PPC runs the server on the caller core. Fine-grained modularity beats Linux
  on throughput/latency despite more IPC hops — IPC cost is not the bottleneck.
- **QNX:** microkernel (sched + IPC + signals) + resource-manager servers
  presenting pathname namespaces; supervisor (`procnto`/`slogger`).
- **MINIX 3:** per-driver isolation + **reincarnation server** restarts crashed
  drivers; Intel ME ships it — most-deployed µk in the world.
- **Zircon/Fuchsia:** handles ≈ capabilities; channels/sockets/vmos for bulk;
  driver framework in userspace.
- **1st-gen lesson (Mach):** 100 µs IPC killed the design; **L4 (1993):**
  register-passing + direct switch → <1 µs. NexIOS already has the analogue
  (`SEND_FAST/RECV_FAST/SEND_SYNC_FAST`, #11) — keep it, add donation.

## 2. Current architecture inventory (as built)

| Subsystem | Now (in-kernel) | Target |
|---|---|---|
| Scheduler | O(1) bitmap 128 lvl, RMS + sporadic budget (`SS(2,10,0)` vfsd, `SS(3,10,0)` iocd), EDF peek, affinity (#61) | Keep; **add** SC donation + passive + timeout (#190) |
| IPC | per-task `MessageQueue`, `send/recv/send_sync` + FAST register path (#11), `BufferPool` zero-copy, endpoints, SHM caps, notify/event/queue/mutex | Keep core; **add** reply-object donation chain, SPSC bulk channels per server |
| Memory | PMM + `MemPool` + `BufferPool` global pools; Untyped carve (#1), Frame caps (#106), pager protocol (#107), CoW planned (#162) | Kernel paths allocation-free; **userspace frame/region manager** (#191); creation via Untyped retype through mem-mgr |
| IRQ | `IrqDelivery` slots, IrqCap (#2), NOTIFY mode (#7), MSI-X vectors (#10), IOAPIC/APIC | Kernel routes only; **userspace IRQ mux** + IRTE (#192); IRQ/MMIO numbers are cap invocations, never ambient |
| Time | kernel timer wheel, tick, `CLOCK_GETTIME/NANOSLEEP/TIMER_CREATE/TIMERFD` (#76, currently `sys_unimplemented`) in-kernel | tick + replenishment in kernel; **timer server** (#194); land #76 as server-RPC stubs first |
| VFS/FS | `vfs.cpp` core + 7 backends + fd-table + `vfsd` k-task prio 20 `send_sync` authorize | fd-table + authorize stay; backends + block server → userspace (#47) |
| Drivers | AHCI/virtio-blk-net/PCI/kbd/RTC/fb/serial in-kernel; completion ISRs (#64/#65) | userspace PDs on Irq+MMIO+DMA caps (#48 owns fb MMIO+IRQ caps + PD isolation); DMA per-domain IOMMU (#4/#9, #168) |
| Console/FB | `Terminal` UC pixel loops + `Klog/Dmesg` singletons in-kernel | terminal **emulation policy** PD over SHM with bounded ops; kernel keeps panic `raw_write` (#51; #48 owns the fb device caps, #51 owns the emulation — no double ownership) |
| procfs | in-kernel over VFS | procfs PD over external VFS (#54) |
| Threads | `fork/clone` deep-copy, TLS base (#74), no shared-space threads | cap-gated address-space share + pthread on picolibc (#52/#53) |
| Caps | GRANT/COPY/REVOKE/MINT/RETYPE, Mmio/Irq/IOMMU/Msix/Frame/Endpoint/Pager/Death | **add** grant/revoke cascade proof (#50), SC + SchedControl caps (#190) |
| Init/supervision | `reboot_from_table` + `g_task_defs`, death notify (#105) | **static arch + supervisor** with restart (#193) |
| Syscalls | **86** (`MAX_SYSCALL=86`) | **38 numbered mechanism entries** (35 retained + 3 new SC; see §3) |

## 3. Target kernel API — normative numbered list (reconciles the ~20/~38 mismatch)

"Core" = mechanism-only invocations. Target = **38 numbered entries**
(35 retained existing numbers + 3 new SC; FAST 74/75/76 are distinct retained
numbers inside the set):
`YIELD=0`, `SEND=1`, `RECEIVE=2`, `SEND_SYNC=3`, `GET_TICKS=5` (tick only),
`EXIT=6` (self only), `NOTIFY=27`, `NOTIFY_WAIT=28`,
`CAP_GRANT=51`, `CAP_COPY=52`, `CAP_REVOKE=53`, `CAP_MINT=54`,
`CAP_RETYPE=56` (Untyped carve; **creation path for frames — FRAME_CREATE=63
is REMOVED**, frames are created via retype through mem-mgr),
`FRAME_MAP=64`, `FRAME_UNMAP=65` (mechanism only),
`DEATH_WATCH=66`, `DEATH_RECV=67`, `DEATH_UNWATCH=68`,
`PAGER_REGISTER=69`, `PAGER_RECV=70`, `PAGER_MAP=71`, `PAGER_ABORT=72`,
`PAGER_UNREGISTER=73`, `ABI_VERSION=84`, `TLS_SET=85`,
`IOPORT_GRANT=55` (cap mechanism, x86_64; policy in mem-mgr),
`MMIO_MAP=61`, `MMIO_UNMAP=62` (cap-invocation mechanism only on an MmioCap;
mapping policy in mem-mgr),
`IOMMU_MAP=59`, `IOMMU_UNMAP=60` (mechanism only; fail-closed without domain),
`IRQ_REGISTER=57`, `IRQ_WAIT=58` (route-only mechanism on an IrqCap;
masking/sharing policy in irq-mux #192),
`SC_BIND/SC_UNBIND/SC_DONATE` (new, numbers TBD at API freeze; pointer-free
args per syscall.hpp:188-193 review; FAST-mask membership decided then).
Count: 8 thread/IPC + 5 CAP + 2 FRAME-mech + 3 DEATH + 5 PAGER + 2 misc +
1 IOPORT-mech + 2 MMIO-mech + 2 IOMMU-mech + 2 IRQ-mech + 3 FAST + 3 SC = **38**.
`syscall_surface` asserts exactly this set; any addition needs SIL 3
justification (feeds Phase 10 #80).

Explicit disposition of all other current numbers (nothing left ambiguous):
`PRINT=4` → removed (replaced by log-server RPC; kernel keeps panic
`raw_write` only, no PRINT syscall); `CREATE_MAILBOX=7`/`DESTROY_MAILBOX=8` →
removed (legacy; endpoints via caps); `EVENT_SET=29`/`EVENT_WAIT=30` → sync
server RPC (notify-based); `SIGNAL=31`/`SIGRETURN=32` → process-server RPC
(#164); `ALARM=33` → timer-srv RPC; `GETTOD=34` → timer-srv RPC;
`UNAME=35` → supervisor RPC (static info); `PAUSE=36` → removed (use
NOTIFY_WAIT / timeout-recv); `OPEN=9`/`READ=10`/`CLOSE=11`/`FSTAT=12`/
`WRITE=13`/`LSEEK=14`/`IOCTL=15`/`READDIR=16`/`STAT=17`/`DUP=18`/`DUP2=26`/
`PIPE=25`/`MKDIR=41`/`UNLINK=42`/`RMDIR=43`/`CHDIR=19` → VFS-server RPC;
`FORK=21`/`EXEC=20`/`WAITPID=22`/`KILL=24`/`GETPID=23` → process-server RPC;
`CLOCK_GETTIME=80`/`NANOSLEEP=81`/`TIMER_CREATE=82`/`TIMERFD_CREATE=83` →
timer-srv RPC (land as stubs first, §7.0); `BRK=44`/`GETRLIMIT=45`/
`SETRLIMIT=46` → memory-manager RPC; `BUF_ALLOC=37`/`BUF_FREE=38`/
`BUF_MAP=39`/`BUF_UNMAP=40` → SHM-manager RPC; `GETRANDOM=47` → entropy-server
RPC; `KLOG=48` → log-server RPC; `SET_AFFINITY=77`/`GET_AFFINITY=78`/
`TIMES=79` → split per §3.1; `SEND_FAST=74`/`RECV_FAST=75`/
`SEND_SYNC_FAST=76` → retained (FAST variants of 1/2/3);
`REBOOT=49`/`HALT=50` → supervisor-gated (cap-checked, supervisor PD only —
never open syscalls).

### 3.1 Affinity/accounting split (SET/GET_AFFINITY, TIMES)

Mechanism stays in-kernel: `set_affinity_err` admission probe +
`queue_target` + `read_times` (scheduler.hpp:229-242 semantics), SC→core
binding honouring #167 pinning (isolated RT cores excluded from balancer,
mandatory pinning). Policy (placement decisions, load thresholds) moves to a
scheduler policy server. The probe is the enforcement point: unpinned or
isolated-core violations fail with a typed error, never silently.

## 4. Server architecture (static, supervised)

```
boot SDF ──► supervisor ─┬─ mem-mgr (#191) ── frames/regions for all (owns_untyped)
                         ├─ proc-mgr ── fork/exec/waitpid, pid+gen
                         ├─ vfs-srv (#47) ─┬─ initrd/tmpfs/fat32 backends
                         │                 └─ block-srv (AHCI/virtio, DMA caps)
                         ├─ drv PDs (#48: fb/kbd/rtc/net MMIO+IRQ caps, PD isolation)
                         ├─ term-srv (#51: emulation policy over SHM, bounded UC ops)
                         ├─ procfs-srv (#54) ── over vfs-srv
                         ├─ timer-srv (#194) ── POSIX time over tick+timeout
                         ├─ irq-mux (#192) ── policy routing + IRTE
                         ├─ sync-srv ── EVENT_* over NOTIFY
                         └─ log-srv ── Klog/Dmesg sink, drop/meter, panic path stays in kernel
```
Rules: strictly-sequential servers; bulk via SPSC SHM channels (LionsOS);
control via `send_sync` (+FAST for ≤48 B); passive servers bill the caller
(#190); every server supervised: death-notify → restart → cap re-derive →
client rebind (MINIX reincarnation; #193). No ambient authority: boot derives
exactly the SDF-declared caps; `boot_composition` test enforces it.

## 5. IPC design (performance contract)

- Keep FAST register path (48 B, pointer-free, fail-closed oversize clamp).
- Add donation: `Call` lends caller SC to passive server; reply returns it
  exactly once; overrun → timeout fault with consumed amount; handler resets
  server to receive state and error-replies the client (MCS §3.5/IPCP ceiling
  option). FAST ≤48 B path is donation-accounted but scheduler-bypassed;
  exemption measured against §9.4.
- Bulk: per-channel SPSC rings in SHM (`BufferPool` transfer/map as
  mechanism, policy in servers); single-producer/single-consumer, lock-free;
  head/tail atomic in ALL contexts (CODING_STYLE §11.6 all-or-nothing).
- Budget: `extra_refills`-style bound on replenishment fragmentation;
  documented per-server (timer server highest client rate bounds its handler
  budget — MCS thesis lesson).

## 6. Capability plan + enforced invariants (CODING_STYLE §§11–12, with points)

Existing caps stay; add **SC cap** (budget/period, bind/unbind/donate,
per-core `SchedControl`) and complete **#50** (take-grant copy/mint,
recursive revoke with depth bound, owner-death drain on every teardown path,
generation-tagged stored pointers). Enforced at these points:
- (a) No spinlock across `reschedule()` on the donate path
  (`ipc.cpp:block_sender/wake_sender`, endpoint locks, CNode/SC locks released
  before BLOCKED + `reschedule()`; lock order scheduler_lock_ → endpoint).
- (b) BLOCKED always dequeued, incl. passive-receive and timeout-reset paths
  (`dequeue_ready` before `reschedule()`; `recv_wait_arm` false → stay RUNNING).
- (c) Generation tags on the donation chain, waiter arrays, `bound_receiver_`,
  `last_sender_`, mailbox slots; validate at use; reject TERMINATED **and**
  REAPED (dead-state filter).
- (d) Wakers own wakeup: endpoint dispose, SC unbind, timeout-reset-to-receive,
  and server-crash paths drain/wake or error-reply every blocked client.
- (e) OOM-as-error on retype/bind/donate/enqueue-full/refill-full (typed
  errors; ENSURE only for impossible-by-design).
- (f) PI/donation revertible + symmetric: boosts/donations cleared on
  wake/unlock/reply; priority mutation via scheduler helper only.

## 7. Migration (strangler, dual-mode, per-server gates)

0. Create milestone `[v0.7.x] Transition to Microkernel` first (ROADMAP.md
   currently un-milestoned for Phase 8); record kernel text budget + §3
   38-entry target at approval. Land #76 (80..83, now `sys_unimplemented`)
   as server-RPC stubs **before** the timer-srv flip.
Per server, in order: timer → log → procfs → term/fb → drivers → block →
VFS backends → process management → memory manager. Each step:
(a) server PD + Kconfig shim default OFF; (b) loopback/parity tests
(kernel vs server path byte-identical); (c) fault-injection
(crash/overrun/revoke) green; (d) SIL 3 audit APPROVED; (e) flip default,
keep kernel fallback one milestone. Kernel code is deleted **only after**
that server's gate (audit APPROVED + full `test-full` debug AND release
green + ResourceTracker delta-0); §9.1 restated accordingly — no
delete-first reading is valid.

## 8. Test plan (maps to issues; all stub-first)

Every new test starts as `JARVIS_TEST_PASS()` stub with
`JARVIS_REGISTER_TEST` + `test_expected_counts.hpp` update, real asserts only
after the stub lands; every test runs under `test_isolate` snapshot/restore
with `ResourceTracker::check` delta-0 — including kill/restart cycles
(`crash_restart`, `restart_no_leak`), whose drain paths must respect
`is_current_on_any_cpu` (scheduler.hpp:838-846, never remove a running task).
- #47: `vfs_server_restart`, `block_dma_isolated`, `vfs_authorize_toctou`.
- #48: `driver_crash_isolated`, `irq_no_ambient`, `timer_drift_bound`.
- #49: `syscall_surface` (asserts exact §3 38-entry set), `core_footprint`
  (kernel text ≤ budget), `no_kernel_fs` (fault-inject fs path → kernel fs
  code absent from backtrace).
- #50: `revoke_cascade`, `stale_use_fails`, `death_drain`.
- #51: `fb_fault_isolated`, `scroll_bounded`, `log_no_block`.
- #52: `thread_share_mem`, `thread_exit_isolated`, `tls_isolation`.
- #53: `pthread_n1_stress`, `cond_wakeup`, `cancel_safe`.
- #54: `proc_over_vfs`, `pid_reuse_safe`, `watchdog_visible`.
- #190 (`cap_schedctx`): donation billing, overrun timeout, no-leak return.
- #191 (`mm_usermgr`): zero-alloc IPC, OOM-as-error, partition isolation.
- #192 (`irq_mux`): owner-only routing, revoke-drain, MSI-X entry isolation.
- #193 (`srv_supervisor`): crash-restart, boot composition, restart-no-leak.
- #194 (`srv_timer`): no-kernel-block sleep, 50-timer multiplex, RTC-degrade.
Related: #76 moves in-kernel POSIX time out; #105 death-notify is the
supervisor primitive; #106/#1 frame primitives feed #191; #107 pager becomes
a server client; #162–#166 userspace subsystems sit atop; #167 pinning constrains
SC→core binding; #168 SMMU extends DMA isolation to ARM.

## 9. Acceptance

1. Kernel = §3 38-entry mechanism API only, reached **exclusively** via the
   §7 per-server sequence (Kconfig default-OFF → parity → flip → fallback one
   milestone → delete after gate). No kernel deletion before its server gate.
2. All §8 classes green, debug `test-full` 20/20 + release 18/18+2skips,
   `make build` Errors 0, ResourceTracker delta 0 on restart cycles.
3. Fault demo: kill any server → rest live, supervisor restarts, clients
   rebind (recorded serial log).
4. IPC overhead ≤10% vs in-kernel path on send_sync-heavy load (LionsOS bar).
5. SIL 3 APPROVED per issue; this spec planner-APPROVED (stable draft).

## 10. Risks

- Donation accounting overhead (MCS reports up to 13–35% IPC worst-case with
  strict limits) → mitigate with FAST-path accounting + measure §9.4.
- Replenishment fragmentation under preemption storms → bound refills,
  forfeit policy documented per server.
- POSIX compat creep back into kernel → §3 list is normative; additions
  rejected without audit.
- Driver DMA without IOMMU on legacy paths → fail-closed: no DMA cap without
  domain (x86 VT-d now, SMMU #168 keeps ARM honest).
