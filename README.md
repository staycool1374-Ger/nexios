<!-- SEO Metadata: NexIOS is an independent custom operating system project. It is not affiliated with Cisco NX-OS, Cisco IOS, NixOS, or Nexos. Dedicated C++20 x86_64 AARCH64 RISCV64 Hard Real-Time Kernel. -->
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
  <img src="https://img.shields.io/badge/tests-1768%20debug%20%7C%2089%20release-2ea44f?style=flat-square" alt="Tests"/>
  <img src="https://img.shields.io/badge/C++20-freestanding-00599C?style=flat-square&logo=cplusplus" alt="C++20 Freestanding"/>
  <img src="https://img.shields.io/badge/arch-x86__64%20%7C%20ARM64%2FRISCV%20planned-1f425f?style=flat-square" alt="x86_64"/>
  <img src="https://img.shields.io/badge/security-capability--based%20%28CSpace%29-fb7185?style=flat-square" alt="Capability Security"/>
  <img src="https://img.shields.io/badge/scheduling-hard%20real--time-critical?style=flat-square" alt="Hard Real-Time"/>
  <img src="https://img.shields.io/badge/process-SIL%203%20inspired-orange?style=flat-square" alt="SIL 3 inspired process"/>
  <img src="https://img.shields.io/badge/version-v0.5.3-blue?style=flat-square" alt="Version"/>
  <img src="https://img.shields.io/badge/license-GPLv3-blue?style=flat-square" alt="GNU General Public License v3"/>
</p>

---

## Overview NexIOS

A freestanding C++20 multi-architecture real-time operating system with zero dynamic heap allocation (Slab-Allocator) in critical paths and deterministic O(1) scheduling.

Currently a hybrid kernel actively transitioning toward a capability-based microkernel.

* **Target:** x86_64, ARM64 & RISC-V
* **Language:** Freestanding C++20 (`-fno-exceptions`, `-fno-rtti`, zero `libc`/`libstdc++`)
* **Status:** v0.5.3 — User Space Enhancements (1768 debug tests, 89 release tests: ramdiskd microkernel service, initrd /bin + sh PATH/export, runelf envp/fork fixes, send_sync reply contract, CheckedPtr overflow guard)
* **License:** GPLv3

NexIOS RTOS is an independent, ground-up implementation of a real-time operating system.

---

## Diagram of actual implementation

<img src="diagram.png" alt="Diagram of actual implementation"/>

---

## Who Needs This?

NexIOS combines MMU process isolation and capability-based security (CSpace) with a straightforward workflow:  **your application is just an ELF file on the system — NexIOS keeps it running on time, every time.**

Write your control logic in C, C++, or Rust and execute it directly via the POSIX-compliant NexIOS shell. Applications can be dynamically loaded from storage or secondary media without needing a system reboot. If an application fails, simply reload and restart it. Handy for a rapid application development (RAD) system without re-flashing like: 

* **Robotics & Motion Control:** High-frequency servo and actuator control loops requiring strict sub-millisecond execution windows.
* **Industrial & Home Automation:** Multi-tasking controllers (e.g., climate regulation, energy management, sensor pipelines) where individual services can be updated on the fly without stopping core safety routines.
* **Rapid Edge Prototyping:** Dropping and testing updated user binaries on real-time target hardware without re-flashing the underlying kernel.

If you have been looking for exactly this kind of system and want to help test or build it: 
**this project is GPLv3 open source, and contributors are welcome.**
Please read through open roadmap issues before submitting major architectural changes.

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

## Roadmap & History

- **Current work:** [GitHub Milestones](https://github.com/staycool1374-Ger/nexios/milestones) — open items tracked as Issues.
- **Full backlog:** ~80 aspirational roadmap items as [GitHub Issues](https://github.com/staycool1374-Ger/nexios/issues) (labeled `feature`, grouped by phase).

Done roadmap archived in `prompts/README_done.md`.

---

## Recent Release Highlights

### **v0.5.3 — User Space Enhancements**
* **Microkernel Services Live:** ramdiskd block service (8×2 MiB, chunked IPC, lazy mapping) with ramdisk_fs mount/file ops + loadelf/runelf support, host XMODEM/upload tooling, and the watchdogd supervision daemon.
* **Userspace Usability:** initrd /bin hierarchy, sh PATH lookup + export, English shell strings + unified terminal core, and Rust no_std hello-world user ELF with pinned toolchain.
* **Loader & IPC Hardening:** runelf 3-arg envp canary fix, fork admission scoping for user children, send_sync reply-buffer ABI contract, CheckedPtr count-overflow guard, and fat32 stable identity for TOCTOU re-resolve.
* **Full Gates Green:** 1768/1768 debug, 89/89 release, `make build` Errors 0 (smp stall + deadline tick-race cleared by identical-binary rerun evidence).

### **v0.5.2 — User-ELF Bring-up to Run**
* **End-to-End User ELF Execution:** `runelf` loads and runs general ELFs as strictly periodic user tasks with deadline registration, Liu-Leyland + memory-budget admission, TLS setup, and `/etc/rc` integration.
* **Kernel Hygiene H1–H8:** pure-refactor cleanup across memory (PMM/VMM/MemPool), scheduler/task lifecycle, and sync primitives — zero functional change, SIL 3 PRE+POST approved per part.
* **Debugger Surface:** `runelf --debug` integration, stop-event snapshot budget export, and the bare-metal debug-discipline audit (gaps filed as follow-ups).
* **Full Gates Green:** 1696/1696 debug, 85/85 release, `make build` Errors 0.

### **v0.5.1 — Bring-up Multi-Arch Boot**
* **Three Architectures Boot to Production:** aarch64 and riscv64 join x86_64 with real production boot paths — page-table/HHDM bring-up, PCI discovery, per-arch syscall ABI conformance tests, an EL0 fork smoke test, and riscv64 U-mode user tasks.
* **Kernel Debugger Foundations:** the debugd groundwork lands — kernel debug syscalls (read/write regs/mem, attach), stop-event routing with breakpoint shadows, per-arch stepping, and an RSP parser core with mock-transport tests.

---

## Community, Support & Q&A

Have questions about the architecture, CSpace capabilities, or real-time scheduling guarantees?

* **Q&A Thread:** Check out or participate in our central [Q&A Discussion #189](https://github.com/staycool1374-Ger/nexios/discussions/189).
* **Issue Tracker:** Report bugs or discuss upcoming features on [GitHub Issues](https://github.com/staycool1374-Ger/nexios/issues).

---
  
## Build & Quick Start

#### Using Docker (recommended)

```bash
docker pull ghcr.io/staycool1374-ger/nexios-env:v0.5.3
docker run --rm -it ghcr.io/staycool1374-ger/nexios-env:v0.5.3
# inside the container:
make build
make execute-test x86_64 debug debug_syscall
```

> **Apple Silicon / arm64 hosts:** `docker pull` fetches the arm64 image
> by default, which emulates x86_64 via TCG (slower, same results). If
> you hit `BdsDxe ... Not Found` followed by `Start PXE over IPv4`, your
> image predates the multiarch GRUB fix — pull again, or force the amd64
> image explicitly (runs emulated, needs Rosetta/QEMU user-static):
> `docker pull --platform linux/amd64 ...` /
> `docker run --platform linux/amd64 ...`.

Run a gate directly without an interactive shell:

```bash
docker run --rm ghcr.io/staycool1374-ger/nexios-env:v0.5.3 \
    make execute-test x86_64 debug debug_syscall
```

Pin a different snapshot explicitly (default is the image's own tag):

```bash
docker run --rm -it -e NEXIOS_TAG=main ghcr.io/staycool1374-ger/nexios-env:v0.5.3
```
#### Running in QEMU Emulator

* **Interactive Debug Mode (Default x86_64):** Launches the kernel in QEMU with serial debug output active.
  ```bash
  make run-debug-mode
  ```
* **Cross-Architecture Targeting:** Append the target architecture (`x86`, `arm`, or `riscv`) to emulate other platforms:
  ```bash
  make run-release-mode arm    # Boots AArch64 target in QEMU
  make run-release-mode riscv  # Boots RISC-V 64 target in QEMU
  ```

#### Upload files into ramdisk (XMODEM)

One command streams a host file into the running ramdiskd, with progress
on both ends (host `\r` line + receiver `[rx n=..]` lines). Needs a
running target on TCP serial — the proven path is q35+OVMF without the
IDE drive (see `docs/specs/xmodem.md`):

```bash
# 1. Boot with TCP serial, then start the receiver once:
qemu-system-x86_64 -cdrom debug/nexios-rtos.iso -m 256M \
  -serial tcp:127.0.0.1:4465,server,nowait -machine q35 -cpu max \
  -drive if=pflash,format=raw,readonly=on,file=<edk2-x86_64-code.fd> \
  -display none
# (in the shell over that serial:) source /tests/xmodem276.txt

# 2. Upload (framing auto-selects raw vs base64; --start-block required):
tools/upload-ramdisk.py myfile.bin --start-block 1000
# upload done bytes=20480 blocks=40 framing=raw naks=0 spill=55 time=127.0s
```

Exit codes: `0` ok, `1` dialogue/handshake, `2` usage/count mismatch,
`3` receiver abort (nothing written), `4` timeout. `--selftest` checks
the host logic without a target. Details: `docs/specs/xmodem.md`
(operator section) and issue #313.
