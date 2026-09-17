# NexIOS RTOS — Development Roadmap

**Build:** v0.4.8-dev | **Last Release:** v0.4.7 | **Completed milestones:** v0.2.x — v0.4.7

> **NOTE (2026-09-17):** the user explicitly overrode the "pointers only, no
> work items" rule for this file and requested a full sorted listing.
> This file was generated from GitHub (`gh` API). Source of truth for state
> remains the GitHub Issues / Milestones (`staycool1374-Ger/nexios`) —
> do NOT check off items here; close them on GitHub. Regenerate (don't
> hand-edit) when refreshing.

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development
- **v0.4.8 Enhance Deadline Scheduling** ([milestone 8](https://github.com/staycool1374-Ger/nexios/milestone/8)): #20, #22, #23, #24 open (#19, #21 closed).
- **v0.5.2 Userspace Subsystems** (NEW, [milestone 15](https://github.com/staycool1374-Ger/nexios/milestone/15)): #162–#166 open.
- Past release records: see `ROADMAP_done.md` (v0.2.x — v0.4.7).

## Topic Coverage Map (subsystem review 2026-09-17)
| Topic | Status | Issues |
|---|---|---|
| IOMMU / SMMU DMA protection | covered (closed) | #4, #9 |
| Demand paging / CoW | **filed new** | #162 |
| stac/clac (ARM64 PAN) | covered (closed) | #5 |
| Paging / IRQ (RISC-V) | covered (open) | #29, #152 |
| PLT/GOT runtime resolution | covered (closed) | #95 |
| POSIX signals (sigaction/sigreturn) | **filed new** | #164 |
| Stack unwinding (user-space) | **filed new** | #163 |
| procfs enhancement | covered (open) | #54 |
| tmpfs enhancement | **filed new** | #165 |
| Mounting dynamically loaded drivers | **filed new** | #166 |
| Distributed runqueue load balancing | covered (closed) | #61 |
| Zero-copy IPC buffer pools | covered (closed) | #11, #14, #106 |

## v0.4.2 — User-Space Infrastructure (CLOSED)
- #1 [closed] Untyped child-split + sub-range carve — exhaustion-model retype.
- #2 [closed] IRQ caps + user-space IRQ delivery (IrqCap).
- #3 [closed] MMIO caps + fine-grained I/O delegation.
- #4 [closed] IOMMU DMA protection (VT-d / AMD-Vi / SMMU) — identity-IOVA tables.
- #5 [closed] MP-4.4 aarch64 PAN/PXN enablement — incl. PAN-sysreg stac/clac.
- #6 [closed] Pending audits/refactorings under audits/ (P0–P8).
- #7 [closed] User-space IRQ delivery system (NOTIFY mode).
- #8 [closed] Fine-grained hardware I/O delegation (user MMIO maps).
- #9 [closed] IOMMU DMA protection layer (live VT-d enablement).
- #10 [closed] MSI-X vector infrastructure.
- #91 [closed] bug: ISR_NOERR misclassification (#VE/#HV error codes).
- #103 [closed] aarch64 deep_copy L3 descriptor bits.
- #153 [closed] [S1] AP trampoline staging clobbers GRUB multiboot info.

## v0.4.3 — Syscall Fastpath + FPU / Test-Enhancement Determinism (CLOSED)
- #11 [closed] In-register IPC fastpath (SEND/RECEIVE/SEND_SYNC).
- #12 [closed] External pager protocol (#PF delegation).
- #13 [closed] Fault recovery & crash supervisor.
- #14 [closed] Capability shared-memory granules (zero-copy).
- #15 [closed] Priority-ordered blocked-sender wakeup.
- #92 [closed] Design paper: syscall-fastpath.md.
- #93 [closed] Design paper: fpu-context.md.
- #101 [closed] Hard deterministic RT measurement under QEMU.
- #102 [closed] Scheduler test assertions with time measurement.
- #105 [closed] Fault isolation: external pager + crash supervisor.
- #106 [closed] Zero-copy data path (SHM granules + ordered wake).
- #107 [closed] External pager protocol paper requirement.
- #119 [closed] AhciDriver::probe allocation (> MemPool class).
- #120 [closed] aarch64 build break (iommu unused-variable).

## v0.4.3 — Test-Coverage Completion (CLOSED)
- #108 [closed] Coverage gap: AHCI deep paths.
- #109 [closed] Coverage gap: procfs nodes.
- #110 [closed] Coverage gap: keyboard scancodes.
- #111 [closed] Coverage gap: pipe blocking semantics.
- #112 [closed] Coverage gap: initrd cpio parser.
- #113 [closed] Coverage gap: ACPI table parsing (incl. DMAR).
- #114 [closed] Coverage gap: tmpfs corruption tests.
- #115 [closed] Coverage gap: GDT layout.
- #116 [closed] Coverage gap: RTC date math.
- #117 [closed] Coverage gap: virtio_blk request path.
- #118 [closed] Coverage gap: serial driver logic.

## v0.4.4 — APIC + SMP / ELF Shared Objects (CLOSED)
- #25 [closed] Local/IO APIC, X2APIC, per-CPU GDT/TSS, AP startup.
- #26 [closed] TPR interrupt prioritization, core isolation.
- #27 [closed] Per-CPU asm for isr_nesting_depth.
- #60 [closed] hhdm_modified_ (VAR-17) re-audit.
- #85 [closed] Test coverage: v0.4.x (18 modules).
- #94 [closed] Design paper: per-cpu-smp.md.
- #95 [closed] Design paper: elf-shared-libs.md (PLT/GOT, DT_NEEDED).
- #141 [closed] Shell build_canonical_path drops leading '/'.
- #142 [closed] Coverage denominator (duplicate C1/C2 symbols).
- #143 [closed] Memory test adoption (safe_copy user-task gates).
- #144 [closed] IrqThread teardown (ResourceTracker leak).
- #145 [closed] Queue/Mutex uninitialised members (page fault).
- #146 [closed] Queue()/Mutex() semifinal fix for #145.
- #147 [closed] Coverage Phase A merge + HTML tree fixes.
- #148 [closed] Threaded-IRQ dispatch wedge.
- #149 [closed] safe_copy fault-recovery resume loop (S1).

## v0.4.5 — Kernel Half Merge + Cache Coloring (CLOSED)
- #61 [closed] Distributed runqueues, RT load balancer, affinity syscalls.
- #62 [closed] Cache coloring allocator, SMP locks, WCET re-audit.
- #96 [closed] Design paper: kernel-half-merge.md.
- #151 [closed] fpu_owner per-CPU migration.
- #154 [closed] executed_ticks charged to running task only (was: all READY).

## v0.4.6 — TLB Shootdown (CLOSED)
- #63 [closed] Epic: PCID/INVPCID/lazy shootdown/IPI batching/profiling.
- #155 [closed] Reaper (PID 1) wait-for-child instead of spinning.
- #156 [closed] PCID-tagged CR3 switches.
- #157 [closed] Selective INVPCID invalidation.
- #158 [closed] Lazy shootdown + deferred-free quarantine.
- #159 [closed] IPI batching for shootdown delivery.
- #160 [closed] Shootdown latency profiling.
- #161 [closed] Race conditions and refactoring review.

## v0.4.7 — Enhance HRT (CLOSED)
- #16 [closed] High-resolution monotonic clock (TickSource + ns_monotonic).
- #17 [closed] Per-CPU event-timer wheel (static slots + generations).
- #18 [closed] Wheel-armed bounded receive (VULN-W3 close).

## v0.4.8 — Enhance Deadline Scheduling (ACTIVE)
- #19 [closed] Deadline-aware preemptive scheduling (DM + global EDF).
- #20 [open] Enforced admission control (Liu-Layland gate, was advisory).
- #21 [closed] Per-task execution-time metering (SYS_TIMES).
- #22 [open] Aperiodic & deferrable servers.
- #23 [open] SMP admission extension (partitioned EDF).
- #24 [open] Kernel self-test of admission bounds.

## v0.4.9 — IRQ Blocking + Audit (OPEN)
- #64 [open] AHCI completion ISR.
- #65 [open] virtio-blk completion ISR.
- #66 [open] Bounded-blocking audit.

## v0.4.10 — Test-Coverage Closure, Areas < 80% (ACTIVE)
- #124 [closed] kernel/vfs (45.6%).
- #125 [closed] services/shell+terminal (40.3%).
- #126 [open] lib (57.0%).
- #127 [open] kernel/memory (57.1%).
- #128 [closed] kernel/debug (8.3%).
- #129 [closed] kernel/profiling (36.4%).
- #130 [open] kernel/driver (63.6%).
- #131 [open] top-level kernel/ (60.7%).
- #132 [closed] kernel/sync (69.9%).
- #133 [open] kernel/iommu (71.1%).
- #134 [open] kernel/syscall (71.4%).
- #135 [open] kernel/daemon (75.0%).
- #136 [open] kernel/core (77.8%).
- #137 [open] kernel/cap (78.0%).
- #138 [open] kernel/net (53.6%).
- #139 [open] kernel/boot (66.7%).

## v0.5.0 — picolib + abi (OPEN)
- #67 [open] Document trap/IRQ numbers (syscall ABI).
- #68 [open] Register conventions (syscall ABI).
- #69 [open] syscall.h public header (syscall ABI).
- #70 [open] Versioned syscall table (syscall ABI).
- #71 [open] POSIX syscall stubs (picolibc).
- #72 [open] Build picolibc (picolibc integration).
- #73 [open] Makefile integration (picolibc).
- #74 [open] TLS on context switch (picolibc).
- #75 [open] Verify (picolibc integration).
- #76 [open] POSIX time API (clock_gettime/nanosleep/timer_create/timerfd).

## v0.5.1 — Bring-up Multi-Arch Boot (OPEN)
- #28 [open] aarch64 production boot path.
- #29 [open] riscv64 production boot path (Sv39 MMU, PLIC, UART, timer).
- #30 [open] Per-arch syscall ABI conformance tests.
- #104 [open] aarch64 fork/clone EL0 smoke test.
- #152 [open] riscv64 map/unmap hhdm/identity flags (follow-up of #60).

## v0.5.2 — Userspace Subsystems (NEW, OPEN)
- #162 [open] Demand paging / CoW subsystem — fault classifier, per-VMA policy, CoW refcounts, pager integration.
- #163 [open] Stack unwinding in user-space — frame-pointer walk, crash/profiler integration.
- #164 [open] POSIX signals: sigaction/sigreturn — disposition table, delivery, restore.
- #165 [open] tmpfs enhancement — large files, fsync semantics, lookup cache, fewer fixed limits.
- #166 [open] Mounting dynamically loaded drivers — ELF load, cap-gated bind, registry lifecycle.

## v0.6.0 — User-ELF Bring-up to Run (OPEN)
- #41 [open] Per-task software watchdog (SYS_WATCHDOG_CREATE).
- #43 [open] Idle-task safety monitors (RAM march, ALU check, utilisation).
- #45 [open] Deterministic boot (safety systems).
- #46 [open] ELF & kernel-image authenticity verification.
- #77 [open] runelf end-to-end: wire take_completed() into runelf/sys_exec.

## Un-milestoned Tracks (OPEN)
- Phase 7 Safety Systems (0.6.x): #40 ICH9/HPET watchdog + NMI; #42 wait-for-graph deadlock detection; #44 ARINC-653 partitioning.
- Phase 8 Microkernel Transition (0.7.x–0.8.x): #47 externalise VFS & block I/O; #48 externalise drivers; #49 kernel reduction; #50 cap-based security; #51 externalise console & framebuffer; #52 shared-address-space threads; #53 pthread library; #54 userspace procfs server.
- Phase 9 Hardware Drivers & Protocols (0.9.x): #55 full TCP/IP + NIC; #56 USB stack; #57 seqguard layer; #58 zero-copy network DMA rings; #59 certification artifacts.
- Phase 10 v1.0.0 Release Gate: #78 capability security; #79 deterministic HRT scheduling; #80 kernel minimality; #81 safety completeness; #82 full test gate; #83 SIL 3 audit approval; #84 userspace completeness.
- RPi4 bare-metal bring-up: #31 aarch64 build cleanup; #32 board config; #33 U-Boot path; #34 PL011 console; #35 GIC-400; #36 RPi4 MMU layout; #37 per-arch test gate; #38 storage for initrd/ELFs; #39 mailbox/framebuffer.
- Coverage trackers: #86 v0.5.x; #87 v0.6.x; #88 v0.7.x; #89 v0.8.x; #90 v0.9.x; #123 coverage Phase A (gcov).

## Un-milestoned (CLOSED, historical)
- #97 workflow state machine + gate script + FEEDBACK transition.
- #99 multi-arch compile-clean (aarch64 + riscv64).
- #100 aarch64 PMM OOM panic (meminit ordering).
- #121 sampling profiler SMPL capture fix.
- #122 per-class instrumented coverage + merged report.
- #150 release stringop-overflow in checked_ptr sweep.

## Future Roadmap (Aspirational)
- **Phase 4.6 (0.4.x):** User-Space Driver Infrastructure & Hardware Isolation
- **Phase 4.7 (0.4.x):** Time, Deterministic Scheduling & Bounded I/O; Interrupt-Driven I/O & Bounded Blocking
- **Phase 5 (0.4.x):** SMP + Multicore
- **Phase 6 (0.5.x):** System Integration / Userspace ABI (syscall ABI, picolibc, POSIX time, runelf, multi-arch production boot, Raspberry Pi 4)
- **Phase 7 (0.6.x):** Safety Systems
- Later phases through Phase 10 (v1.0.0 release gate)

### Design Papers (docs/specs/)
| Paper | Topic | Target |
|---|---|---|
| [`docs/specs/syscall-fastpath.md`](../docs/specs/syscall-fastpath.md) | Syscall dispatch via static asm jump table | v0.4.3 |
| [`docs/specs/fpu-context.md`](../docs/specs/fpu-context.md) | FPU/SIMD fixed-offset save areas + lazy reentrancy | v0.4.3 |
| [`docs/specs/exception-table-audit.md`](../docs/specs/exception-table-audit.md) | Exception vector audit (#VE/#HV fix, bug #91) | v0.4.3 |
| [`docs/specs/per-cpu-smp.md`](../docs/specs/per-cpu-smp.md) | Per-CPU foundation + SMP bring-up (#94) | v0.4.4 |
| [`docs/specs/elf-shared-libs.md`](../docs/specs/elf-shared-libs.md) | DT_NEEDED resolution (#95) | v0.4.4 |
| [`docs/specs/kernel-half-merge.md`](../docs/specs/kernel-half-merge.md) | Kernel-half page-table merge (#96) | v0.4.5 |

Browse the backlog: https://github.com/staycool1374-Ger/nexios/issues?q=is%3Aopen+label%3Afeature
