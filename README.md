<!-- SEO Metadata: NexIOS is an independent custom operating system project. It is not affiliated with Cisco NX-OS, Cisco IOS, NixOS, or Nexos. Dedicated C++20 x86_64 Hard Real-Time Kernel. -->
<p align="center">
  <img src="nexios-rtos-logo.png" alt="NexIOS RTOS Logo" width="600"/>
</p>

<h1 align="center">NexIOS RTOS</h1>
<p align="center">
  <em>A deterministic, safety-critical real-time operating system built from scratch in freestanding C++20.</em>
</p>
<p align="center">
  <strong>🌐 Project website: <a href="https://nexios-2.jimdosite.com">https://nexios-2.jimdosite.com</a></strong>
</p>

<p align="center">
  <a href="https://github.com/staycool1374-Ger/nexios/actions/workflows/ci.yml"><img src="https://github.com/staycool1374-Ger/nexios/actions/workflows/ci.yml/badge.svg" alt="CI Build"/></a>
  <img src="https://img.shields.io/badge/tests-1363%20debug%20%7C%2085%20release-2ea44f?style=flat-square" alt="Tests"/>
  <img src="https://img.shields.io/badge/C++20-freestanding-00599C?style=flat-square&logo=cplusplus" alt="C++20 Freestanding"/>
  <img src="https://img.shields.io/badge/arch-x86__64%20%7C%20ARM64%2FRISCV%20planned-1f425f?style=flat-square" alt="x86_64"/>
  <img src="https://img.shields.io/badge/security-capability--based%20%28CSpace%29-fb7185?style=flat-square" alt="Capability Security"/>
  <img src="https://img.shields.io/badge/scheduling-hard%20real--time-critical?style=flat-square" alt="Hard Real-Time"/>
  <img src="https://img.shields.io/badge/process-SIL%203%20inspired-orange?style=flat-square" alt="SIL 3 inspired process"/>
  <img src="https://img.shields.io/badge/version-v0.5.0-blue?style=flat-square" alt="Version"/>
  <img src="https://img.shields.io/badge/license-GPLv3-blue?style=flat-square" alt="GNU General Public License v3"/>
</p>

---

## Overview NexIOS

A freestanding C++20 real-time operating system for x86_64 with zero dynamic heap allocation in critical paths and deterministic O(1) scheduling.

Currently a monolithic kernel (47 syscalls via `int 0x82`), actively transitioning toward a capability-based microkernel.

* **Target:** x86_64 (ARM64 & RISC-V in preparation)
* **Language:** Freestanding C++20 (`-fno-exceptions`, `-fno-rtti`, zero `libc`/`libstdc++`)
* **Status:** v0.5.0 — picolibc + ABI (1564 debug tests, 85 release tests: syscall ABI freeze, picolibc integration, TLS, POSIX time API, loader verification)
* **License:** GPLv3

NexIOS RTOS is an independent, ground-up implementation of a real-time operating system.

---

## What's the plan

The core vision of NexIOS is straightforward: **your application is just an ELF file on the system — NexIOS keeps it running on time, every time.**

Write your control logic in C, C++, or Rust and execute it directly via the POSIX-compliant NexIOS shell. Applications can be dynamically loaded from storage or secondary media without needing a system reboot. If an application fails, simply reload and restart it:

* **MMU-Isolated Processes:** Every user ELF binary runs in its own address space under a deterministic hard real-time schedule.
* **Fault Interception & Recovery:** If a process crashes or faults, the kernel catches the exception and isolates the failure. The rest of the operating system stays up, while only the individual process restarts.

This architecture enables deterministic execution for time-critical workloads, such as:

* **Robotics & Motion Control:** High-frequency servo and actuator control loops requiring strict sub-millisecond execution windows.
* **Industrial & Home Automation:** Multi-tasking controllers (e.g., climate regulation, energy management, sensor pipelines) where individual services can be updated on the fly without stopping core safety routines.
* **Rapid Edge Prototyping:** Dropping and testing updated user binaries on real-time target hardware without re-flashing the underlying kernel.

As far as known, no other open-source RTOS today combines this exact workflow — load-and-run an unmodified user ELF with deterministic timing guarantees, capability-based security, and crash isolation. That gap is the destination of this project.

**Honest status:** this is where the journey goes, not where we are today.
The x86_64 kernel core already proves the concept (MMU-isolated processes, background ELF loader, capability security). The pieces that make the maker workflow real — ARM and RISC-V board bring-up, end-to-end user-ELF loading from storage, and automatic fault recovery — are open roadmap items tracked as GitHub Issues. The road is long, but every step on it is concrete and reachable.

If you have been looking for exactly this kind of system and want to help test or build it: **this project is GPLv3 open source, and contributors are welcome.** See [Call for Contributions](#call-for-contributions).

---

## What's different about NexIOS

**The core design goal of NexIOS is to execute any user application (ELF binary) as a dedicated, fully isolated user-task — scheduled deterministically, isolated in its own address space, and sandboxed without re-compiling.**

Most hobby and embedded RTOS projects (FreeRTOS, Zephyr, STK) are **scheduler libraries**: threads share one address space and any task can corrupt any other. NexIOS takes the operating-system path:

- **Isolated process execution** — every user ELF runs as a dedicated task in its own 4-level page-table space with guard pages, scheduled deterministically, sandboxed without recompilation.
- **Capability-based security (CSpace)** — tasks hold *capabilities* to kernel objects (endpoints, frames, IRQs, Untyped memory). Grant/copy/mint/revoke semantics; no ambient authority: a syscall without the matching capability fails. Most monolithic kernels check *who you are*; NexIOS checks *what you can prove*.
- **Deterministic core** — zero dynamic heap allocation on real-time paths, O(1) scheduling decisions, RAII-enforced interrupt windows (`IrqGuard`).
- **AI-orchestrated development process** — every change passes a mandatory three-agent pipeline (planner → developer → independent SIL 3 auditor) with diff-based audits, in-kernel regression gates, and full audit-trail documentation. NexIOS doubles as a long-running case study: how far can structured LLM orchestration go in building a safety-critical system?

---

## What's the user's advantage

NexIOS can be used as a rapid application development (RAD) system without re-flashing.
By streaming user binaries via `rsync` (or a network/serial stream) into an in-memory loop device (`/dev/ram0`), developers achieve sub-second test iterations with complete fault isolation.

### Key Architectural Advantages

* **Zero-Flash Iteration:** No wear on SD cards, no kernel reflashing, zero reboot overhead.
* **Instant Dynamic Execution:** User-space ELFs are parsed, mapped into 4-level page tables, and assigned explicit CSpace capability domains dynamically.
* **Hard Fault Isolation:** If a deployed binary triggers a memory fault (e.g., null pointer dereference), the ARM64 MMU traps the exception. The kernel revokes the task's CSpace capabilities and reclaims memory pools while the OS, shell, and loop device remain fully operational.

---

### 1. Build the User Binary
Compile your C++20 freestanding application against the NexIOS syscall headers using the cross-toolchain:

`aarch64-none-elf-g++ -O2 -std=c++20 -fno-exceptions -fno-rtti -Wl,-T user_task.ld main.cpp -o my_app.elf`

### 2. Stream to RAM-Disk and load binary
Transfer the compiled ELF binary directly to a mounted loop device on the running target:

`rsync -avz --progress my_app.elf nexios@192.168.1.50:/tmp/my_app.elf`
`nexios> loadelf /tmp/my_app.elf`

### 3. Execute via POSIX Shell
Run the application dynamically from the NexIOS console:

`nexios> runelf `

### 4. Crash Recovery & Hot-Fix Loop
If the user application crashes:

`[KERNEL FAULT] Core 1: Data Abort at EL0 (FAR: 0x0000000000000000)`
`[CSPACE] Revoking capabilities for PID 4...`
`[REAPER] Task 4 terminated cleanly. Kernel resources reclaimed.`
`nexios>`

Fix the code on your host, re-run `rsync`, and restart the task from the shell.

---

## Demo

<p align="center">
  <a href="https://www.youtube.com/watch?v=foApKYTFSlE">
    <img src="https://img.youtube.com/vi/foApKYTFSlE/sddefault.jpg" alt="NexIOS demo video — watch on YouTube" width="720"/>
  </a>
</p>
<p align="center">
  <em>NexIOS demo — real-time kernel for safety (click to watch on YouTube)</em>
</p>

<p align="center">
  <img src="docs/nexios-architecture.png" alt="NexIOS kernel architecture" width="820"/>
</p>

---

## Key Features

* **Zero-Allocation Critical Paths:** TCBs, IPC mailboxes (`MessageQueue`, `Notify`, `EventGroup`), and virtual memory metadata rely on pre-allocated slab allocators (`MemPool`).
* **RAII Concurrency Guards:** Scoped guards (`IrqGuard`) statically enforce `cli`/`sti` boundaries at compile time to eliminate dangling critical sections.
* **Rewind-Based Testing:** State-capture (`capture_state()`) and restoration hooks allow an automated test-suite to run inside QEMU via state rewinds without reboots.

---

## Microkernel Transition (In Progress)

* **Phase 7 (v0.7.x):** VFS (`vfsd`) and block I/O (`iocd`) externalized to isolated Ring 3 servers behind IPC gateways.
* **Phase 8 (v0.8.x):** Kernel reduced to scheduler, IPC, page-table manager, and IRQ routing. Shell, init, VFS, and drivers run as capability-bearing Ring 3 servers.

---

## Roadmap & History

- **Current work:** [GitHub Milestones](https://github.com/staycool1374-Ger/nexios/milestones) — open items tracked as Issues.
- **Full backlog:** ~80 aspirational roadmap items as [GitHub Issues](https://github.com/staycool1374-Ger/nexios/issues) (labeled `feature`, grouped by phase).
- **Implementation history (what's already done):** [`prompts/ROADMAP_done.md`](prompts/ROADMAP_done.md) — the complete audit trail of every shipped milestone from v0.3.7 through v0.4.10 (CSpace capability security, User-Space Infrastructure caps/IOMMU/MSI-X, SMP bring-up, Cache coloring, TLB Shootdown, High-Resolution Time, Deadline Scheduling, IRQ Blocking, and more), each entry with root-cause analyses, commit ranges, and validated test-gate results.

Full roadmap archived in `prompts/ROADMAP.md` and `prompts/README_done.md`.

---

## Recent Release Highlights

### **v0.4.10 — Test-Coverage Closure (Areas < 80%)**
* **Every Weak Spot Covered:** All 16 sub-80% coverage areas closed with real tests — vfs, services/shell, lib, memory, debug, profiling, driver, top-level kernel, sync, iommu, syscall, daemon, core, cap, net, boot — plus dead-code removal (`compiler_rt` clz/ctz) shrinking the denominator.
* **Tests That Find Bugs:** Closure testing exposed and fixed real defects — dropped syscall results for kernel-task callers, silent `lseek` success on bogus `whence`, IP length-underflow OOB read, missing ICMP byte-swaps, a coverage-boot panic at the checked-ptr fault boundary.
* **Harness & Arch Hardening:** `hal` page-table mapping hygiene, record-but-continue verdict parsing for benign panic text, and aarch64/riscv64 link-green (TLB-purge fallbacks, EL0 fault handler, gated `compiler_rt`).

### **v0.4.9 — Interrupt-Driven I/O & Live System Monitoring**
* **No More Disk Polling:** Storage drivers (AHCI/virtio-blk) now sleep while the hardware works and wake on hardware interrupts — bounded waits replace core-blocking spins, with fail-closed polling fallbacks.
* **Verified Blocking Discipline:** Every driver wait path audited — bounded loops or scheduler-mediated waits, never unbounded spins — with the guarantees pinned in the binding-invariants spec.
* **See Inside the Machine:** New `cpuinfo` and `top` shell commands show per-CPU load, task placement and affinity, real period usage vs WCET, zombie processes, and system-vs-user load split — built for SMP correctness checks.
* **Idle Shell & SMP by Default:** Interactive input sleeps in notification waits (near-0% idle CPU), the release shell boots 2-CPU SMP out of the box, and load displays follow the wall clock.

### **v0.4.8 — Deadline-Aware Scheduling & Enforced Admission Control**
* **Deadline-Driven Dispatch:** Tasks with the earliest deadlines run first (Earliest-Deadline-First), with priorities auto-assigned from deadlines — time-critical work provably meets its timing guarantees.
* **Guaranteed Admission:** The kernel now refuses new real-time tasks that would overload any CPU core (idle-time and background workloads exempt), so admitted tasks can never miss deadlines due to overcommit — including across task migration and multi-core placement.
* **Flexible Aperiodic Service + Self-Verification:** Deferrable and background server modes handle bursty, non-periodic work beside classic sporadic servers, and every boot re-verifies the scheduling guarantees with a built-in admission self-test.

### **v0.4.7 — High-Resolution Time & Bounded Waits**
* **Precise System Clock:** A calibrated, high-resolution monotonic timebase provides sub-millisecond accuracy for deadlines and latency measurements.
* **Efficient Timer Management:** A constant-time event queue powers system timeouts, driver deadlines, and watchdog timers without scanning overhead.
* **No More Indefinite Blocking:** Inter-process message receives now support bounded timeouts, guaranteeing every blocking call returns — with a message or a timeout — instead of waiting forever.

### **v0.4.6 — System Responsiveness & Memory Efficiency**
* **Event-Driven Idle Cleanup:** Terminated process reaping is handled via an event-driven wait loop rather than CPU spinning, preserving system resources and improving responsiveness.
* **Faster Process Context Switches:** Hardware-assisted memory management (PCID/INVPCID) prevents unnecessary CPU cache flushes during application context switches.
* **Multi-Core Overhead Reduction:** Cross-core memory invalidation requests (TLB shootdowns) are batched to avoid interrupting active real-time workloads.

### **v0.4.5 — Multi-Core Real-Time Scheduling**
* **Distributed Runqueues:** Per-CPU task queues eliminate global scheduler lock contention on multi-core systems.
* **Real-Time Load Balancing & Core Pinning:** Time-critical tasks can be deterministically distributed or strictly bound to specific CPU cores (`SYS_SET_AFFINITY`).
* **Cache-Coloring Memory Allocator:** A tailored physical memory allocator minimizes L1/L2 CPU cache collisions.

### **v0.4.4 — SMP Bring-Up & Shared Libraries**
* **Multi-Core Bootup:** Complete multi-core initialization using Inter-Processor Interrupts (IPIs).
* **Dynamic Linking (ELF Shared Objects):** Applications can share common library code (`DT_NEEDED`), significantly reducing memory footprints.
* **Robust Boot Handshake:** Automatic relocation of overwritten bootloader staging data prevents page faults during initial bootup.

### **v0.4.3 — Hardware Driver Stability & Coverage**
* **Extended Hardware Diagnostics:** Expanded test coverage for storage controllers (AHCI/SATA), real-time clocks (RTC), and ACPI tables on bare metal.
* **Cross-Platform Hardening:** Elimination of silent build and runtime defects across both x86_64 and ARM64 architectures.

### **v0.4.2 — User-Space Drivers & Hardware Isolation**
* **IOMMU DMA Protection:** Hardware drivers running in user-space are isolated via VT-d to prevent faulty DMA accesses from compromising system integrity.
* **Fine-Grained Hardware Delegation:** Direct assignment of hardware interrupts (MSI-X/IRQ) and MMIO regions to user-space drivers without kernel privilege escalation.
* **Dynamic Capability Management:** Granular hardware rights can be delegated to child processes and deterministically revoked at any time.

### **v0.4.1 — Capability Security Model (CSpace)**
* **Zero Ambient Authority:** Tasks operate under strict capability-based access control, accessing only kernel objects and IPC endpoints explicitly granted to them.
* **Deterministic Resource Teardown:** Automatic, leak-free cleanup of kernel objects enforced through a multi-holder shared reference counting model.
* **Sub-Range Memory Carving:** Applications can safely subdivide and retype untyped memory ranges independently.

---

## Build & Quick Start

### Prerequisites

```bash
sudo apt install build-essential git wget xorriso dosfstools \
    x86_64-linux-gnu-gcc binutils qemu-system-x86
