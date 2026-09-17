# NexIOS RTOS — Development Roadmap

**Build:** v0.4.11-dev | **Last Release:** v0.4.10 | **Completed milestones:** v0.2.x — v0.4.9 (see `ROADMAP_done.md`)

> **NOTE (2026-09-17):** the user explicitly overrode the "pointers only, no
> work items" rule for this file and requested a full sorted listing
> (2026-09-17: reduced to OPEN items only, closed versions/items removed).
> This file was generated from GitHub (`gh` API). Source of truth for state
> remains the GitHub Issues / Milestones (`staycool1374-Ger/nexios`) —
> do NOT check off items here; close them on GitHub. Regenerate (don't
> hand-edit) when refreshing.

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development
- **v0.4.10 Test-Coverage Closure** ([milestone 10](https://github.com/staycool1374-Ger/nexios/milestone/10)): coverage issues open.
- **v0.5.2 Userspace Subsystems** ([milestone 15](https://github.com/staycool1374-Ger/nexios/milestone/15)): #162–#166 open.
- Past release records: see `ROADMAP_done.md` (v0.2.x — v0.4.9).

## Topic Coverage Map (subsystem review 2026-09-17)
| Topic | Status | Issues |
|---|---|---|
| IOMMU / SMMU DMA protection | covered (closed: #4, #9) | — |
| Demand paging / CoW | **open** | #162 |
| stac/clac (ARM64 PAN) | covered (closed: #5) | — |
| Paging / IRQ (RISC-V) | open | #29, #152 |
| PLT/GOT runtime resolution | covered (closed: #95) | — |
| POSIX signals (sigaction/sigreturn) | **open** | #164 |
| Stack unwinding (user-space) | **open** | #163 |
| procfs enhancement | open | #54 |
| tmpfs enhancement | **open** | #165 |
| Mounting dynamically loaded drivers | **open** | #166 |
| SMMUv3 Stage-2 translation | **open** | #168 |
| Symmetrical core isolation / CPU pinning | **open** | #167 |
| Distributed runqueue load balancing | covered (closed: #61) | — |
| Zero-copy IPC buffer pools | covered (closed: #11, #14, #106) | — |

## v0.4.9 — IRQ Blocking + Audit (RELEASED 2026-09-17, see ROADMAP_done.md)
- #64 [closed] AHCI completion ISR.
- #65 [closed] virtio-blk completion ISR.
- #66 [closed] Bounded-blocking audit.
- #172 [closed] UX: cpuinfo/top commands.

## v0.4.10 — Test-Coverage Closure, Areas < 80% (ACTIVE)
- #126 [open] lib (57.0%).
- #127 [open] kernel/memory (57.1%).
- #130 [open] kernel/driver (63.6%).
- #131 [open] top-level kernel/ (60.7%).
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

## v0.5.2 — Userspace Subsystems (NEW, OPEN)- #162 [open] Demand paging / CoW subsystem — fault classifier, per-VMA policy, CoW refcounts, pager integration.
- #163 [open] Stack unwinding in user-space — frame-pointer walk, crash/profiler integration.
- #164 [open] POSIX signals: sigaction/sigreturn — disposition table, delivery, restore.
- #165 [open] tmpfs enhancement — large files, fsync semantics, lookup cache, fewer fixed limits.
- #166 [open] Mounting dynamically loaded drivers — ELF load, cap-gated bind, registry lifecycle.

## v0.5.3 — Hardware Isolation (NEW, OPEN)
- #167 [open] Symmetrical core isolation / CPU pinning — isolated RT cores, mandatory pinning, IRQ/balancer exclusion (wired into #23).
- #168 [open] SMMUv3 Stage-2 translation — STE/CD programming, CMDQ invalidations, IORT discovery, QEMU virt integration.

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
