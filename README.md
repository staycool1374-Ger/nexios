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
  <img src="https://img.shields.io/badge/C++20-freestanding-00599C?style=flat-square&logo=cplusplus" alt="C++20 Freestanding"/>
  <img src="https://img.shields.io/badge/arch-x86__64-1f425f?style=flat-square" alt="x86_64"/>
  <img src="https://img.shields.io/badge/scheduling-hard%20real--time-critical?style=flat-square&logo=clockifier" alt="Hard Real-Time"/>
  <img src="https://img.shields.io/badge/concurrency-RAII%20guarded-2ea44f?style=flat-square" alt="RAII Concurrency"/>
  <img src="https://img.shields.io/badge/tests-881%20passing-2ea44f?style=flat-square" alt="881 Tests Passing"/>
  <img src="https://img.shields.io/badge/license-GPLv3-blue?style=flat-square" alt="GNU General Public License v3"/>
</p>

---

# Overview NexIOS

A freestanding C++20 real-time operating system for x86_64 with zero dynamic heap allocation in critical paths and deterministic O(1) scheduling.

Currently a monolithic kernel (47 syscalls via `int 0x82`), actively transitioning toward a capability-based microkernel.

* **Target:** x86_64 (ARM64 & RISC-V in preparation)
* **Language:** Freestanding C++20 (`-fno-exceptions`, `-fno-rtti`, zero `libc`/`libstdc++`)
* **Status:** v0.4.1 — CSpace (Capability-Based Access Control) + Untyped allocator, Memory Protection (MP-1..MP-8), background ELF loader
* **License:** GPLv3

NexIOS RTOS is an independent, ground-up implementation of a real-time operating system.

---

## What's different about NexIOS
The core design goal of NexIOS is to execute any user application (ELF binary) as a dedicated, fully isolated user-task, scheduled deterministically, isolated in its own address space and sandboxed without re-compiling.

---

## Key Features

* **Zero-Allocation Critical Paths:** TCBs, IPC mailboxes (`MessageQueue`, `Notify`, `EventGroup`), and virtual memory metadata rely on pre-allocated slab allocators (`MemPool`).
* **RAII Concurrency Guards:** Scoped guards (`IrqGuard`) statically enforce `cli`/`sti` boundaries at compile time to eliminate dangling critical sections.
* **Rewind-Based Testing:** State-capture (`capture_state()`) and restoration hooks allow an automated test-suite to run inside QEMU via state rewinds without reboots.

---

## Microkernel Transition (In Progress)

* **Phase 7 (v0.7.x):** VFS (`vfsd`) and block I/O (`iocd`) externalized to isolated Ring 3 servers behind IPC gateways.
* **Phase 8 (v0.8.x):** Kernel reduced to scheduler, IPC, page-table manager, and IRQ routing. Shell, init, VFS, and drivers run as capability-bearing Ring 3 servers.

Full roadmap archived in `ROADMAP.md` and `README_done.md`.

---

## Build & Quick Start

### Prerequisites

```bash
sudo apt install build-essential git wget xorriso dosfstools \
    x86_64-linux-gnu-gcc binutils qemu-system-x86
```

### Build & Run

```bash
git clone <repo-url>
cd os
make help           # showing list of usage
make debug          # Debug build
make qemu-iso       # Launch in QEMU with serial console
make release        # Optimised release build (no tests)

# Testing targets (QEMU)
make execute-test x86 debug selftest  # Safe class (CI gate)
make execute-test x86 debug all-1    # First half
make execute-test x86 debug all-2    # Second half
make execute-test x86 debug <class>  # Specific test class

# Renode simulation (multi-arch)
make run-renode RENODE_ARCH=x86_64   # x86_64 via SeaBIOS+ISO
make renode-test          # Renode CI validation
```

### Build Architecture

```
  [ Userspace Apps ] <─── Ring 3 Isolation
────── [ Syscall Interface: int 0x82 (47 syscalls) ] ──────
  [ Shell (Kernel Task, 36 built-ins) ]  [ RMS Scheduler        ]
  [ VFS / Initrd / Devfs / Procfs / FAT32 ] [ Priority IPC Mailbox]
  [ Virtual Memory (VMM, 4-level PT)    ]  [ Notify & Event Groups]
  [ O(1) PID→TCB Hash Table             ]  [ Priority Inheritance ]
  [ Physical Memory (PMM, Buddy Alloc)   ]  [ Slab Alloc (MemPool) ]
  [ Hardware: Serial, KBD, Framebuffer,   ]  [ ATA PIO, PIT, RTC    ]
  [ PCI, Virtio, ACPI                    ]  [ RNG, FPU Lazy Switch ]
  [ Gcov, Driver Registry, Integrity     ]  [ Deadlock Detection   ]
═════════════ Monolithic Kernel (Ring 0) ═════════════
```

---

## Call for Contributions

NexIOS RTOS is an architectural project first and a feature project second. We are seeking contributions from engineers who like to participate.
If this aligns with your engineering philosophy, open an issue or pull request.

---

## License

NexIOS RTOS is free software: you can redistribute it and/or modify it under the terms of the **GNU General Public License**, either version 3 of the License, or (at your option) any later version. See [`LICENSE.txt`](LICENSE.txt) for the full text.
