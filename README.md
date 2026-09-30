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
  <img src="https://img.shields.io/badge/version-v0.5.1-blue?style=flat-square" alt="Version"/>
  <img src="https://img.shields.io/badge/license-GPLv3-blue?style=flat-square" alt="GNU General Public License v3"/>
</p>

---

## Overview NexIOS

A freestanding C++20 real-time operating system for x86_64 with zero dynamic heap allocation in critical paths and deterministic O(1) scheduling.

Currently a monolithic kernel (47 syscalls via `int 0x82`), actively transitioning toward a capability-based microkernel.

* **Target:** x86_64 (ARM64 & RISC-V in preparation)
* **Language:** Freestanding C++20 (`-fno-exceptions`, `-fno-rtti`, zero `libc`/`libstdc++`)
* **Status:** v0.5.1 — Bring-up Multi-Arch Boot (1610 debug tests, 85 release tests: aarch64/riscv64 production boot, per-arch syscall ABI conformance, EL0 fork smoke, debugd phases 1–2)
* **License:** GPLv3

NexIOS RTOS is an independent, ground-up implementation of a real-time operating system.
---

## Diagram of actual implementation

<img src="diagram.png" alt="Diagram of actual implementation"/>

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

```bash
aarch64-none-elf-g++ -O2 -std=c++20 -fno-exceptions -fno-rtti -Wl,-T user_task.ld main.cpp -o my_app.elf
```

### 2. Stream to RAM-Disk and load binary
Transfer the compiled ELF binary directly to a mounted loop device on the running target:

```bash
rsync -avz --progress my_app.elf nexios@192.168.1.50:/tmp/my_app.elf
nexios> loadelf /tmp/my_app.elf
```

### 3. Execute via POSIX Shell
Run the application dynamically from the NexIOS console:

```bash
nexios> runelf my_app.elf
```

### 4. Crash Recovery & Hot-Fix Loop
If the user application crashes:

```text
[KERNEL FAULT] Core 1: Data Abort at EL0 (FAR: 0x0000000000000000)
[CSPACE] Revoking capabilities for PID 4...
[REAPER] Task 4 terminated cleanly. Kernel resources reclaimed.
nexios>
```

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

Done roadmap archived in `prompts/README_done.md`.

---

## Recent Release Highlights

### **v0.5.1 — Bring-up Multi-Arch Boot**
* **Three Architectures Boot to Production:** aarch64 and riscv64 join x86_64 with real production boot paths — page-table/HHDM bring-up, PCI discovery, per-arch syscall ABI conformance tests, an EL0 fork smoke test, and riscv64 U-mode user tasks.
* **Kernel Debugger Foundations:** the debugd groundwork lands — kernel debug syscalls (read/write regs/mem, attach), stop-event routing with breakpoint shadows, per-arch stepping, and an RSP parser core with mock-transport tests.

### **v0.5.0 — picolibc + ABI**
* **Frozen System-Call Interface:** The kernel/userspace contract is pinned at v1.0 — trap numbers, register conventions and a versioned syscall table (86 calls) in a single public header, so applications built today keep working tomorrow.
* **Real C Library for Applications:** picolibc is integrated as the userspace C library — POSIX stubs with proper error reporting, thread-local storage switched on every context switch, and a 5/5 verification program proving it end to end.
* **POSIX Time Services:** Applications get wall-clock and timer APIs (`clock_gettime`, `nanosleep`, `timer_create`, `timerfd`) backed by the high-resolution clock and event-timer wheel, covered by 10 dedicated conformance tests.

---

## Community, Support & Q&A

Have questions about the architecture, CSpace capabilities, or real-time scheduling guarantees?

* **Q&A Thread:** Check out or participate in our central [Q&A Discussion #189](https://github.com/staycool1374-Ger/nexios/discussions/189).
* **Issue Tracker:** Report bugs or discuss upcoming features on [GitHub Issues](https://github.com/staycool1374-Ger/nexios/issues).
* **Contributing:** Pull requests are welcome! Please read through open roadmap issues before submitting major architectural changes.

---
  
## Build & Quick Start

### Prerequisites

Ensure you have the required host toolchain, C++20 freestanding compiler, assembly utilities, bootloader tools, and QEMU emulators installed:

#### Ubuntu / Debian
```bash
sudo apt update
sudo apt install -y \
    build-essential \
    git \
    wget \
    xorriso \
    dosfstools \
    nasm \
    grub-pc-bin \
    grub-common \
    gcc-x86-64-linux-gnu \
    g++-x86-64-linux-gnu \
    qemu-system-x86 \
    qemu-system-arm \
    qemu-system-misc
```

#### macOS (Homebrew)
```bash
brew install qemu xorriso llvm nasm
```

---

### Step-by-Step Setup & Execution

#### 1. Clone the Repository
Clone the NexIOS source tree and navigate into the root directory:
```bash
git clone [https://github.com/staycool1374-Ger/nexios.git](https://github.com/staycool1374-Ger/nexios.git)
cd nexios
```

#### 1b. Or skip the setup: release Docker image
Every milestone tag (starting v0.5.1) ships a ready-made Ubuntu
environment on GHCR — cross toolchains, QEMU/OVMF, ISO tooling, GDB —
so all gates work with zero host setup. Running it clones that tag
and drops you into the tree. QEMU runs emulated (TCG), so no KVM is
needed on any host (including Apple Silicon, via the arm64 image):

```bash
docker pull ghcr.io/staycool1374-ger/nexios-env:v0.5.1
docker run --rm -it ghcr.io/staycool1374-ger/nexios-env:v0.5.1
# inside the container:
make build
make execute-test x86_64 debug debug_syscall
```

Run a gate directly without an interactive shell:

```bash
docker run --rm ghcr.io/staycool1374-ger/nexios-env:v0.5.1 \
    make execute-test x86_64 debug debug_syscall
```

Pin a different snapshot explicitly (default is the image's own tag):

```bash
docker run --rm -it -e NEXIOS_TAG=main ghcr.io/staycool1374-ger/nexios-env:v0.5.1
```

#### 2. Build Targets

NexIOS utilizes standard C++20 freestanding toolchains across target architectures (`x86_64`, `arm64`, and `riscv64`):

* **Debug Build:** Compiles with debug symbols, kernel trace logs, and assertion checks (`-g -Og -DCONFIG_DEBUG`). Outputs `debug/nexios-rtos.iso`.
  ```bash
  make debug
  ```
* **Release Build:** Executes an optimized production build with static analysis enabled (`-O2 -fanalyzer`), packs the initrd, and outputs `release/nexios-rtos.iso`.
  ```bash
  make release
  ```
* **Clean Tree:** Removes all build artifacts and generated binary images.
  ```bash
  make clean
  ```

#### 3. Run in QEMU Emulator

* **Interactive Debug Mode (Default x86_64):** Launches the kernel in QEMU with serial debug output active.
  ```bash
  make run-debug-mode
  ```
* **Interactive Release Mode (Default x86_64):** Boots the production ISO image in QEMU.
  ```bash
  make run-release-mode
  ```
* **Cross-Architecture Targeting:** Append the target architecture (`x86`, `arm`, or `riscv`) to emulate other platforms:
  ```bash
  make run-release-mode arm    # Boots AArch64 target in QEMU
  make run-release-mode riscv  # Boots RISC-V 64 target in QEMU
  ```

---

### Testing & Validation

NexIOS relies on an automated test-driven pipeline to verify capability controls, kernel memory management, and IPC mechanisms:

* **Kernel Selftest:** Executes the safe in-kernel self-test suite within QEMU.
  ```bash
  make execute-test x86 debug selftest
  ```
* **Full Test Suite:** Runs all 16 aggregate test modules across the current architecture.
  ```bash
  make test-full
  ```
