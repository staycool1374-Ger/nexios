# Jarvis RTOS — Development Roadmap

**Build:** v0.4.0-dev | **Last Release:** v0.3.12 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.3.12)

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.4.0

v0.4.0 milestone work IN PROGRESS (details in `ROADMAP_done.md`
§"Active Development — v0.4.0" for v0.2.x–v0.3.12):
- **Memory Protection Phase 4.5 (MP-1..MP-8)** — private kernel-half page
  tables, deep-copy fork, MMU red-zones, software canaries, SMEP/SMAP,
  verification suite (MP-1..MP-6, MP-8 DONE 2026-08-13/14; MP-7 deep-copy fork
  and MP-4.4 aarch64 PAN/PXN OPEN — see BUGS.md pml4_clone crash).
- **Background ELF Loader** — deadline-safe chunked loader (elf_loader 8/8,
  SIL 3 APPROVED; **elf_loader `wait_loader_idle` flake OPEN** (BUGS.md)).
- **H2 Deferred-Switch Race** — **RE-OPENED** (BUGS.md; ~50% hang at
   `ipc_send_sync_roundtrip`; commit `71b3a088` fixed orphan-enqueue but not
   stale-iret-frame root cause).
- **KernelObject Shared-Reference-Count Foundation** — 0.4.1 CSpace
  prerequisite.

Blockers for v0.4.0 completion:
- **MP-4.4 — aarch64 PAN/PXN (DEFERRED)** — no-op stubs in place; requires an
  aarch64 boot path + test class before it can be enabled.
- **Open bug fixes per `BUGS.md`** — `elf_loader` `wait_loader_idle` flake,
  `pml4_clone` CR3-corruption crash, H2 deferred-switch race RE-OPENED.


## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.3.12 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak, Fine-Grained Lock & Safety-Guardrail Enforcement).

---

## Future Roadmap (Aspirational)
> **Version guide (0.4.x → 1.0.0):** features are sorted by dependency, not
> desire — time/scheduling primitives (0.4.7–0.4.9) underpin the userspace ABI
> (0.5.x), safety (0.6.x), the microkernel transition (0.7.x–0.8.x) and the
> driver/protocol phase (0.9.x). v1.0.0 is defined at the bottom of this
> section as the "full blown hard real-time microkernel" release gate.
> **NOTE:** v0.4.0 is NOT yet complete; the prerequisite gate below must clear
> before 0.4.x feature work proceeds.

| Version | Theme | Key deliverables |
|---|---|---|
| 0.4.x | Primitives & foundation | CSpace, user drivers, zero-copy IPC, SMP, time/event timers, deadline-aware scheduling + enforced admission, interrupt-driven I/O |
| 0.5.x | Userspace ABI & multi-arch | Syscall ABI, picolibc, POSIX time API, runelf E2E, aarch64/riscv64 production boot + PAN/PXN |
| 0.6.x | Safety systems | Watchdogs, deadlock detection, idle monitors, temporal partitioning, deterministic boot |
| 0.7.x–0.8.x | Microkernel transition | Externalised VFS/drivers, kernel minimality, threads/pthread, userspace procfs |
| 0.9.x | Drivers & protocols | TCP/IP, USB, zero-copy NIC rings, certification-readiness artifacts |
| 1.0.0 | Release gate | Acceptance criteria at bottom |

**Prerequisite gate (must be 100% resolved before any 0.4.x feature work):**
- [ ] **BUGS.md — H2 deferred-switch race family (RE-OPENED)** — `ipc_send_sync_roundtrip` hang (~50%): reconcile the ROADMAP "RESOLVED" claim with the RE-OPENED BUGS.md entry; pin the stale harness iret-frame writer; green across 16+ repeated `ipc_core` / `all` runs with `CONFIG_DEBUG_IPC_SCHED` **OFF** (release-gate discipline).
- [ ] **BUGS.md — `pml4_clone` CR3-corruption crash** (tests 485–486) — freed page-table CR3=0x1000 under snapshot_restore; resolve the HHDM snapshot-restore ordering blocker.
- [ ] **BUGS.md — `elf_loader` `wait_loader_idle` flake** — loader task stuck BLOCKED non-IDLE after snapshot_restore (same H2 family).
- [ ] **docs/specs/drivers.md §8 — FLAW-01/02/03 (DmaEngine, PingPongDma, virtio-net ring data races)** — add `IrqSpinLockGuard` mutual exclusion; callbacks invoked from stack-captured locals **after** releasing the lock.
- [ ] **FLAW-08 / FLAW-10 (serial unbounded polling, keyboard unbounded drain)** — cap to the bounded-wait discipline.

Rationale: per AGENTS.md, no feature work is stacked on open failures; the H2
family and the DMA races are correctness bugs that would corrupt capability,
IPC and driver work built on top of them.

### Phase 4.6: Microkernel Primitives & Capability Foundation (0.4.x) — prerequisite for 0.7.x Microkernel Transition

#### 0.4.1 — Capability-Based Access Control Architecture (CSpace)
- [ ] **Capability System Specification** — Design object capability structures (CNode, CSlot, CSpace) for tasks, IPC endpoints, physical memory frames, Untyped memory, IRQ controls, and MMIO ranges (eliminating ambient authority).
- [ ] **Capability Lifecycle Primitives** — Implement `SYS_CAP_GRANT`, `SYS_CAP_REVOKE`, `SYS_CAP_MINT`, and `SYS_CAP_COPY` system calls with deterministic reference-counted cleanup.
- [ ] **Untyped Memory Allocator (seL4-style)** — Transition physical allocation away from direct PMM calls to explicit Untyped memory retyping (`Untyped.Retype` into Frame, PageTable, CNode, or Endpoint capabilities).

#### 0.4.2 — User-Space Driver Infrastructure & Hardware Isolation
- [ ] **User-Space IRQ Delivery System** — Hardware IRQ dispatcher that transforms incoming interrupts into Capability-backed IPC Notifications (`sys_irq_register` / `sys_irq_wait`) to eliminate Ring 0 driver execution.
- [ ] **Fine-Grained Hardware I/O Delegation** — Per-task TSS I/O port bitmap management (`sys_ioport_grant`) and capability-gated MMIO page frame mapping for Ring 3 drivers.
- [ ] **IOMMU DMA Protection Layer (VT-d / AMD-Vi / SMMU)** — Program IOMMU translation tables to isolate Ring 3 driver DMA requests strictly to task-owned physical memory frames.
- [ ] **MSI-X Vector Infrastructure** — Per-vector MSI-X allocation with capability-backed delivery (`sys_irq_register` for MSI-X vectors, legacy INTx/PIC fallback); prerequisite for isolated user-space drivers that cannot share legacy pins.

#### 0.4.3 — High-Performance Zero-Copy IPC & Fault Isolation
- [ ] **In-Register IPC Fastpath** — Assembly fastpath for short messages passed via CPU registers (`rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`) skipping full TCB context saves.
- [ ] **External Pager Protocol (#PF Delegation)** — Exception forwarding mechanism routing user-space `#PF` / page faults over IPC to a capability-designated Ring 3 Pager thread.
- [ ] **Fault Recovery & Crash Supervisor** — Asynchronous task-death notifications allowing supervisor processes to catch server crashes, reclaim leaked capabilities, and restart failed drivers cleanly.
- [ ] **Capability Shared-Memory Granules** — Zero-copy ring buffer shared memory mappings (`SYS_SHM_MAP`) backed by capabilities for high-throughput client-server I/O.
- [ ] **Priority-Ordered Blocked-Sender Wakeup** — `wake_sender` pops the **highest-priority** blocked sender instead of FIFO (spec gap: `docs/specs/ipc.md` §6 confirms NOT implemented) under the queue lock; prerequisite for deterministic IPC latency on the zero-copy fastpath.

### Phase 5: SMP + Multicore (0.4.x)
#### 0.4.4 — APIC & SMP Boot
- [ ] Local/IO APIC, X2APIC, per-CPU GDT/TSS, INIT-SIPI AP startup
- [ ] TPR-based interrupt prioritization, core state isolation
- [ ] **Per-CPU asm for `isr_nesting_depth`** — move from the single global symbol to GS-relative access on x86_64 (TPIDR/tp on aarch64/riscv64); the C++ side already uses `__atomic_*` (v0.3.7 PfA-B). CpuContext plumbing (`current_cpu()`) is in place.
- [ ] **`hhdm_modified_` (VAR-17) re-audit** — task-context only today (single-core safe); re-audit under SMP with per-CPU ownership or atomics.

#### 0.4.5 — Per-CPU Scheduling & Cache
- [ ] Distributed run queues, real-time load balancer, SYS_SET/GET_AFFINITY
- [ ] Cache coloring allocator, SMP spinlocks/rwlocks, WCET re-audit

#### 0.4.6 — TLB Shootdown & IPI Reduction
- [ ] PCID, selective INVPCID, lazy shootdowns, IPI batching, latency profiling

### Phase 4.7: Time, Deterministic Scheduling & Bounded I/O (0.4.x)

**Ordering note:** the historical SMP items (0.4.4–0.4.6) are retained in
place; this phase may run in parallel with or be pulled ahead of SMP if the
0.4.5 WCET re-audit or 0.4.6 latency profiling require the high-resolution
clock first.

#### 0.4.7 — High-Resolution Time & Event Timers
- [ ] **High-Resolution Monotonic Clock** — TSC-based monotonic clock with calibration; optional HPET source (`CONFIG_HAS_HPET`); unified tick-source abstraction over APIC TSC-deadline / HPET / PIT fallback with sub-tick resolution for deadlines and metering.
- [ ] **Event-Timer Wheel** — per-CPU O(1) arm/cancel timer queue powering bounded sleeps, driver timeouts, deadline release queues, and watchdog pre-timeouts.
- [ ] **Bounded-Wait Primitive** — blocking paths use the timer wheel for bounded waits; verify and close the unconfirmed `sys_receive` arg3 timeout (VULN-W3, `docs/specs/boundary.md`).

#### 0.4.8 — Deadline-Aware Scheduling & Enforced Admission Control
- [ ] **Deadline-Aware Preemptive Scheduling** — land deadline-monotonic fixed-priority assignment first (bitmap-compatible), then EDF with a deadline-ordered run queue; **`deadline_rush` is NOT implemented per `docs/specs/deadline.md` §7** — this item tracks its implementation from scratch.
- [ ] **Enforced Admission Control** — Liu-Leyland utilization bound **checked** at task create/activate (reject or defer on violation; **spec I-8 currently states advisory-only — this item tracks upgrading to enforced**); per-task WCET/budget admission; memory-budget admission with `CONFIG_MEMORY_BUDGET` enabled by default (`docs/specs/oom-rt.md` §2).
- [ ] **Per-Task Execution-Time Metering (SYS_TIMES)** — high-resolution per-task CPU accounting (extends the TCB `executed_ticks` counter); `SYS_TIMES` syscall exposing per-task/per-period CPU time; feeds admission, WCET validation, and user-space RT profiling.
- [ ] **Aperiodic & Deferrable Servers** — deferrable-server and background-server modes beside the sporadic server (reuse the SS budget/replenish state machine); configurable per task; admission accounting includes server budgets.
- [ ] **SMP Admission Extension** — per-CPU utilization bounds for the 0.4.5 load balancer (partitioned EDF), so migration never violates schedulability.
- [ ] **Kernel Self-Test of Admission Bounds** — boot-time + test-class verification of Liu-Leyland rejection, budget exhaustion, and deadline-miss actions (incl. the `memory_determinism` class, `docs/specs/oom-rt.md` §5).

#### 0.4.9 — Interrupt-Driven I/O & Bounded Blocking
- [ ] **AHCI Completion ISR** — wire a real ISR with per-slot completion records and scheduler wake; `wait_cmd` becomes a blocked wait with a bounded timeout (closes FLAW-05); teardown clears GHC_IE/PORT_IE and takes port locks before freeing CL/RFIS/CT/data (closes FLAW-04 UAF).
- [ ] **virtio-blk Completion ISR** — ISR walking the used-ring plus a wait primitive (closes FLAW-06); integrated with the DmaEngine completion path.
- [ ] **Bounded-Blocking Audit** — every driver path is a bounded blocked wait, never a spin (serial FLAW-08, keyboard FLAW-10; AHCI/virtio timeout values = blocked-wait bounds); `docs/specs/drivers.md` §7.2 binding invariant.

### Phase 6: System Integration / Userspace ABI (0.5.x)

**Priority:** picolibc integration — syscall ABI, TLS, POSIX stubs.

#### Syscall ABI Definition
- [ ] **Document trap/IRQ numbers** — create `src/kernel/syscall/syscall.h` with stable, documented trap vectors and IRQ numbers
- [ ] **Register conventions** — specify register layout for syscall arguments and return values per architecture (x86_64: `rax=num, rdi, rsi, rdx, r10, r8, r9`; aarch64: `x8=num, x0-x5`; riscv64: `a7=num, a0-a5`)
- [ ] **syscall.h public header** — export to userspace, used by both kernel dispatcher and libc stubs
- [ ] **Versioned syscall table** — extend beyond the current 0–50 (`YIELD..HALT`) with the 0.4.x additions (`SYS_CAP_*`, `SYS_TIMES`, `SYS_CLOCK_*`, `SYS_TIMER_*`, `SYS_THREAD_*`, ...) under an explicit ABI version; document the extension rule for new syscalls.

#### picolibc Integration
- [ ] **POSIX syscall stubs** — implement `src/libc/picolib_stubs.c` with wrappers for `_write`, `_read`, `_sbrk`, `_exit`, `_open`, `_close`, `_fstat`, `_lseek`, `_getpid`, `_kill` using `jarvis_syscall()` dispatcher
- [ ] **Build picolibc** — compile with meson as `libc.a` + `libm.a` (static), targeting x86_64-elf
- [ ] **Makefile integration** — link `libc.a`/`libm.a` into kernel image; add build rules for picolibc subproject
- [ ] **TLS on context switch** — every task switch must load the thread-local-storage address into the appropriate base register (`FS` on x86_64, `TPIDR_EL0` on aarch64, `tp` on riscv64). picolibc uses this for `errno` and per-task internal state — no global locks needed.
- [ ] **Verify** — `printf`, `malloc`, `scanf` work from userspace tasks via syscall stubs

#### POSIX Time API Surface
- [ ] **clock_gettime / nanosleep / timer_create / timerfd** — POSIX time and timer syscalls on the 0.4.7 clock + event-timer wheel (`CLOCK_MONOTONIC`, `CLOCK_REALTIME`); picolibc `clock`, `time`, `nanosleep` stubs route here.

#### runelf End-to-End User ELF Execution
- [ ] **Wire `ElfLoader::take_completed()` into the runelf/sys_exec path** — load a general ELF as a strictly periodic user task with deadline registration, enforced Liu-Leyland + memory-budget admission at activation (0.4.8), TLS setup, and `/etc/rc` integration; verify with a user task exercising printf/malloc/IPC/VFS.

#### Multi-Arch Production Boot (aarch64 / riscv64)
- [ ] **aarch64 production boot path** — MMU init, GIC, PL011 serial, arch timer, EL1→EL0 transition; per-arch test gate.
- [ ] **riscv64 production boot path** — MMU init (Sv39), PLIC, UART, `mtime` timer; per-arch test gate.
- [ ] **PAN/PXN enablement** — enable aarch64 PAN/PXN on the production boot path (closes MP-4.4, DEFERRED since 0.4.0); equivalent U/S-mode isolation on riscv64 (sstatus.SUM).
- [ ] **Per-arch syscall ABI conformance tests** — verify the ABI register conventions on each architecture's test gate.

#### Raspberry Pi 4 (BCM2711) Bare-Metal Bring-Up

Target: run NexIOS on a Raspberry Pi 4B bare metal via **U-Boot USB** (kernel
image + DTB loaded from a USB stick) with console on a **SH-U09C5 USB-TTL
UART** adapter (GPIO14=PL011 TX, GPIO15=PL011 RX, 3.3V, 115200 8N1), after the
system is first proven under QEMU virt (`make run-release-mode arm`).

Board memory map (BCM2711, Cortex-A72):
- Peripheral base: `0xFE00_0000` (vs QEMU virt UART `0x0900_0000`).
- PL011 UART: `0xFE20_1000` (GPIO14/15, alt-function 0).
- GIC-400 (**GICv2 only**) — GICD `0xFF84_1000`, GICC `0xFF84_2000` (vs QEMU
  virt `0x0800_0000`/`0x0801_0000`, GICv3-capable).
- RAM: starts at `0x0000_0000` (size from DTB); U-Boot loads `kernel8.img` at
  `$kernel_addr_r` and passes the DTB pointer in `x0` (matches `boot.S`).
- Generic arch timer (CNTP_EL0) and arch timer PPI 30: identical to QEMU virt.

Steps (each lands only after the previous is green; QEMU virt first):
- [ ] **1. aarch64 build-break cleanup (QEMU virt target must compile green)** —
      portability fixes so `make ARCH=aarch64 debug` builds `-Werror`-clean:
      `demo.cpp` `pause` (x86-only asm), `Timer::tsc_freq_hz()` missing on
      aarch64/riscv64, `msr.hpp` unused-param stubs, `ArchPageTable` missing
      x86-compatible index helpers (`pml4_index`/`pdpt_index`/`pd_index`/
      `pt_index`), x86-gated tests referenced unconditionally (test_vmm,
      test_stack_profiler, test_isolate RSP reads), scheduler `context.rsp`
      vs `context.sp_el0` diagnostics.
      **UNFINISHED (2026-08-15):** partial groundwork committed to the tree
      (portable `current_sp()`/`arch::pause()` usage, timer/msr/page-table/
      kernel.cpp fixes) — NOT build-verified end-to-end, NOT SIL 3 audited,
      NOT run on any target. Finish + verify before proceeding.
- [ ] **2. Board-config layer** — introduce `CONFIG_BOARD_AARCH64_RPI4`
      (default: QEMU virt) selecting peripheral base, PL011 base, GIC bases,
      RAM base, kernel load address; parameterize `serial.cpp`,
      `hal/gic.hpp`, `interrupt_controller.cpp`, `boot.S`, linker script.
- [ ] **3. U-Boot boot path** — produce a raw `kernel8.img` (objcopy
      `elf64-littleaarch64` → binary), RPi4 linker script (load at
      `0x0008_0000` / `$kernel_addr_r`), U-Boot sequence
      (`usb start; load usb 0:1 $kernel_addr_r kernel8.img; booti $kernel_addr_r - $fdt_addr`),
      `config.txt` (kernel / arm_64bit=1) if booting without U-Boot.
- [ ] **4. PL011 console bring-up** — PL011 at `0xFE20_1000`, GPIO14/15
      alt-0 pinmux, 115200 8N1; verify banner + shell over the SH-U09C5
      USB-TTL; boot-to-shell on real hardware.
- [ ] **5. GIC-400 (GICv2) init** — enable the existing GICv2 MMIO path with
      RPi4 base addresses (GICD/GICC), arch-timer PPI 30 dispatch; verify
      tick + scheduler preemption over serial.
- [ ] **6. MMU at RPi4 layout** — identity-map RAM, device-map `0xFE00_0000`
      peripherals, higher-half kernel mapping; DTB-based memory-size parse.
- [ ] **7. Per-arch test gate on hardware** — run `selftest` (safe class) on
      the RPi4; then `logging_dmesg`/`scheduler` classes over serial; log
      `test-history.txt` rows for each hardware run.
- [ ] **8. Storage for initrd + user ELFs** — SD (emmc2/sdhci) or USB storage
      for the initrd/user programs (later milestone; serial-only bring-up is
      sufficient for testing).
- [ ] **9. Mailbox / framebuffer (later)** — VC mailbox for power/peripherals,
      then framebuffer console (independent of bring-up).

Hardware notes: 3.3V logic only on GPIO14/15 — do NOT wire the adapter to 5V
pins; connect TX→RX, RX→TX, GND→GND. U-Boot over USB requires a FAT-formatted
stick with `kernel8.img` (and `bcm2711-rpi-4-b.dtb` when DTB is loaded from
the stick).

### Phase 7: Safety Systems (0.6.x)
- [ ] ICH9/HPET hardware watchdog + NMI pre-timeout, PIT fallback, SYS_WATCHDOG_KICK (pre-timeout precision uses the 0.4.7 event-timer wheel)
- [ ] Per-task software watchdog (SYS_WATCHDOG_CREATE), /proc/[pid]/watchdog
- [ ] Wait-for-graph deadlock detection, watchdog-driven recovery, SYS_HEALTH_STATUS
- [ ] Idle-task safety monitors: RAM March C-, CPU ALU verification, utilisation tracking
- [ ] **ARINC-653-Style Temporal & Spatial Partitioning** — major-frame cyclic scheduling with per-partition windows/budgets on the 0.4.8 admission framework; spatial isolation of partitions via capability-gated memory regions.
- [ ] **Deterministic Boot** — bounded boot-time budget from power-on to first RT task; watchdog armed before first RT task activation; boot-order WCET analysis.
- [ ] **ELF & Kernel-Image Authenticity Verification** — load-time hash/Ed25519 verification of user ELFs and the kernel image before first activation.

### Phase 8: Microkernel Transition (0.7.x–0.8.x)
- [ ] Externalise VFS & block I/O to user-space servers
- [ ] Externalise device drivers (keyboard, framebuffer, timer/RTC)
- [ ] Kernel reduction: scheduler, IPC, page-table management, interrupt routing only
- [ ] Capability-based security (SYS_CAP_GRANT / SYS_CAP_REVOKE)
- [ ] **Externalise console & framebuffer** — user-space terminal server with bounded-operation rendering (no uncacheable-heavy kernel scroll paths).
- [ ] **Shared-Address-Space Threads** — `SYS_THREAD_CREATE`/`SYS_THREAD_EXIT` with capability-gated address-space sharing (threads share `page_table_`; private stacks); TCB-per-thread scheduler integration.
- [ ] **pthread Userspace Library** — `pthread_create/join/mutex/cond` on picolibc + thread syscalls for multi-threaded user servers.
- [ ] **Userspace procfs Server** — `/proc/[pid]/stat`, `/proc/[pid]/deadline`, `/proc/[pid]/wcet`, `/proc/[pid]/watchdog`, `/proc/[pid]/capabilities` over the externalised VFS.

### Phase 9: Hardware Drivers & Protocols (0.9.x)
- [ ] Full TCP/IP stack (ARP, IP, ICMP, UDP, TCP) with Ethernet NIC driver
- [ ] USB driver stack (UHCI/EHCI/xHCI)
- [ ] Hot-path secure call sequence layer (<seqguard.hpp>)
- [ ] **Zero-Copy Network DMA Rings** — zero-copy RX/TX rings on `SYS_SHM_MAP` granules with IOMMU-pinned DMA buffers (mitigates FLAW-03 ring-race class via locked ring state; FLAW-03 fix in prerequisite gate is separate).
- [ ] **Certification-Readiness Artifacts** — published WCET ledger per task/syscall/ISR path; fault-injection campaign results; spec↔test↔code traceability matrix; safety-manual skeleton for IEC 61508 SIL 3 / ISO 26262 ASIL D (readiness, not external certification).

### Phase 10: v1.0.0 — "Full Blown Hard Real-Time Microkernel" Release Gate

**Definition.** At v1.0.0 NexIOS is a capability-complete, deterministic,
minimal microkernel: every kernel resource is capability-mediated (zero
ambient authority), scheduling is provably schedulable with **enforced**
admission control and published WCET bounds, the kernel contains only
scheduler, IPC, page-table management and interrupt routing (all services
externalised to user-space servers), it production-boots on three
architectures, and it ships with a complete safety system and an approved
SIL 3 audit.

**Acceptance criteria (all must hold):**
- [ ] **Capability-complete security** — CSpace (0.4.1) mediates every kernel resource (tasks, IPC endpoints, frames, Untyped, IRQs, MMIO); zero ambient-authority syscalls remain; revocation is deterministic and leak-free (zero ResourceTracker delta).
- [ ] **Deterministic HRT scheduling** — deadline-aware scheduling (0.4.8) with enforced admission control; every admitted task has a published, validated WCET bound; deadline-miss actions configurable and tested; no unbounded kernel/driver polling (0.4.9 audit green).
- [ ] **Kernel minimality** — kernel = scheduler, IPC, page tables, interrupt routing only; VFS, block, net, console and device drivers externalised (0.7.x–0.8.x); capability-based security replaces ambient syscall authority.
- [ ] **Multi-arch production boot** — x86_64 (SMP), aarch64 (PAN/PXN on), riscv64 all boot to a user shell with green per-arch test gates.
- [ ] **Safety system completeness** — hardware + software watchdogs, deadlock detection, idle-task monitors, temporal partitioning, deterministic boot, load-time authenticity verification (0.6.x) all operational in release builds (depends on 0.4.8 admission + 0.6.x implementation).
- [ ] **Full test gate green** — debug `all`, release `all`, per-arch gates, and `selftest` at 0 failures with zero ResourceTracker delta; stable `test-history.txt` trend.
- [ ] **SIL 3 audit approved** — auditor sign-off on every modified file; open audit findings count 0; certification-readiness artifacts (0.9.x) current.
- [ ] **Userspace completeness** — picolibc ABI, POSIX time API, runelf E2E with admission control, threads/pthread, and procfs observability all demonstrated by user-space programs.
