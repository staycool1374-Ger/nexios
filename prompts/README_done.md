# Jarvis RTOS — Completed Feature Catalog

## Feature Highlights

### Scheduler & Process Model

| Feature | Status |
|---------|--------|
| Preemptive rate-monotonic scheduling (256 priority levels) | ✓ |
| Periodic and aperiodic task support | ✓ |
| Sporadic Server budget enforcement (C/T parameters, O(1) replenishment) | ✓ |
| O(1) task-ID → TCB hash table (open addressing) | ✓ |
| Process lifecycle: `fork`, `waitpid`, `exit`, `exec` | ✓ |
| ELF64 loader (static binaries from initrd or VFS) | ✓ |
| Per-task `RLIMIT` tracking (`SYS_GETRLIMIT` / `SYS_SETRLIMIT`) | ✓ |
| Lazy FPU/SSE context switch (FXSAVE/FXRSTOR, CR0.TS, #NM trap) | ✓ |
| Task notifications, event groups, alarm signals | ✓ |
| Per-task user page tables with `fork()`-safe PML4 cloning | ✓ |
| Kernel shell with 36+ built-in commands | ✓ |

### Synchronisation & IPC

| Feature | Status |
|---------|--------|
| Mutex with priority inheritance | ✓ |
| Counting semaphore (blocking wait) | ✓ |
| Event group (multi-bit wait) | ✓ |
| Fixed-size message queue (kernel-space, priority bitmap) | ✓ |
| IPC mailbox (send, receive, synchronous rendezvous) | ✓ |
| Buffer pool for zero-copy message payload | ✓ |
| Notification (per-task 32-bit value with wait/clear) | ✓ |
| Pipes (unidirectional byte-stream, `SYS_PIPE` / `SYS_DUP2`) | ✓ |
| Wait-for-graph deadlock detection engine | ✓ |
| RAII `IrqGuard` for interrupt-safe critical sections | ✓ |

### Virtual File System

| Feature | Status |
|---------|--------|
| Unified vnode abstraction with operation vtable | ✓ |
| Per-task file descriptor tables (32 entries) | ✓ |
| Mount table with path-based lookup | ✓ |
| Initrd (cpio newc) — root filesystem at boot | ✓ |
| Devfs — `/dev/tty`, `/dev/null`, `/dev/console`, `/dev/kbd`, `/dev/fb`, `/dev/random` | ✓ |
| Procfs — `/proc/meminfo`, `/proc/[pid]/stat`, `/proc/self`, `/proc/sched` | ✓ |
| FAT32 — full read/write: open, read, write, close, readdir, mkdir, unlink, fstat | ✓ |
| Tmpfs — quota-managed RAM-backed filesystem for `/tmp` | ✓ |
| VFS daemon (`vfsd`) — authorises and proxies filesystem ops for userspace | ✓ |
| IO daemon (`iocd`) — block device I/O request dispatch | ✓ |

### Hardware Enablement

| Feature | Status |
|---------|--------|
| x86_64 long mode, 4-level page tables (PML4 → PDPT → PD → PT) | ✓ |
| Multiboot2-compliant boot | ✓ |
| PIT (1 kHz scheduling tick), RTC (wall clock), HPET (planned) | ✓ |
| PS/2 keyboard (scancode set 2, modifier tracking) | ✓ |
| Serial (COM1, 115200 baud, interrupt-driven) | ✓ |
| Framebuffer (linear via Multiboot2, terminal with status bar) | ✓ |
| ACPI QEMU shutdown port | ✓ |
| ATA PIO (read/write, block device layer) | ✓ |
| PCI bus enumeration (config space access, BAR decoding) | ✓ |
| MSI/MSI-X interrupt support | ✓ |
| Virtio-net (1.0 PCI transport, probe + network stack) | ✓ |
| Virtio-block (1.0 PCI transport) | ✓ |
| Hardware RNG (RDSEED > RDRAND > RDTSC jitter fallback, ChaCha20 CSPRNG) | ✓ |
| Lazy FPU/SSE context switch (FXSAVE/FXRSTOR, #NM trap) | ✓ |

### Kernel Shell (36 Built-in Commands)

| Category | Commands |
|----------|----------|
| Navigation | `cd`, `pwd`, `ls`, `dirs`, `pushd`, `popd` |
| File ops | `mkdir`, `rm`, `rmdir`, `cat` |
| System info | `help`, `uptime`, `version`, `meminfo`, `tasks`, `jobs`, `env`, `times` |
| Process control | `run`, `runelf`, `exit` / `shutdown`, `reboot`, `sleep`, `wait`, `fg`, `bg`, `disown` |
| Shell built-ins | `alias` / `unalias`, `history`, `type`, `which` / `locate`, `source` / `.`, `set`, `read`, `shift`, `export`, `echo`, `printf`, `test` / `[` |
| System control | `modprobe`, `modlist`, `selftest`, `clear`, `trap`, `umask`, `ulimit` |

Features: output redirection (`>`), alias expansion, command history (100-entry ring buffer), directory stack, 32 environment variables, shell execution options (`-x`, `-e`, `-u`), Zsh‑style prompt.

### Test Framework

- **665 test cases** across 80+ test files covering every kernel subsystem
- Run automatically at boot in debug builds; 84 curated tests in release
- Full state snapshot/restore between tests — isolation down to MemPool free-lists and page-table entries
- Leak detection on every test (task counts, page allocations, pool frees)
- QEMU integration: `make test-qemu` captures serial output and validates pass/fail exit codes

### Structured Diagnostics (DMESG)

- **Lock-free SPSC ring buffer** — kernel dmesg subsystem with 4096-entry power-of-2 capacity, no modulo operations
- **Subsystem error enums** — typed error codes for mempool, scheduler, IPC, sync, VFS, syscall, base with string conversion tables
- **Async UART consumer task** — drains ring buffer and formats structured `[DMESG]` log entries without blocking the producer
- **`dmesg` shell command** — prints complete ring buffer to terminal
- **`sys_klog` syscall** — char-buffer compatible formatted output

---

## Architectural Competitive Comparison

The following matrix contrasts the architectural guarantees, safety mechanisms, and design paradigms of Jarvis RTOS against prevailing open-source and commercial real-time operating systems.

| Architectural Dimension | FreeRTOS | Zephyr Project | PREEMPT_RT (RT-Linux) | QNX Neutrino | VxWorks | Jarvis RTOS (v0.2.23) |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **Core Architecture** | Monolithic Library (Flat address space, no isolation) | Monolithic / Modular (Optional MPU-based sandboxing) | Monolithic Macrokernel (Preemptible kernel via locking patches) | Pure Microkernel (Drivers and VFS fully isolated in Ring 3) | Monolithic (With Real-Time Process [RTP] isolation) | **Mid-Transition Microkernel** (Currently moving monolithic Ring 0 services to Ring 3 capability servers) |
| **Primary Language** | C89 / C99 (Manual state tracking, legacy idioms) | C99 / C11 (Extensive preprocessor macro use via Kconfig) | C11 / GNU C (Massive legacy codebase, macro-heavy) | ISO C / C++ (Hosted standard runtime libraries) | C / C++ (Proprietary toolchain-dependent runtime) | **Freestanding C++20** (Zero-overhead objects, compile-time Concepts, no hosted runtime) |
| **Concurrency & Synchronization** | Manual API pairing (`taskENTER_CRITICAL()`, risk of leaks) | Macro-driven locking and explicit unlock tracking | Spinlocks, Mutexes, and raw RT-mutex structures | POSIX threads and synchronization primitives | Task locks, semaphores, and raw spinlocks | **RAII Scoped Guards** (`IrqGuard` lockouts, impossible to leak on early exit blocks) |
| **Memory Allocation** | Fragmentable heaps (`pvPortMalloc()`, no structural bounds) | Configurable dynamic heaps or static block pools | Dynamic allocation via TLSF or standard kernel SLUB/SLAB | Dynamic virtual memory management with standard page pools | Dynamic heap regions with MMU partition protection | **Post-Init Static Pools** (`MemPool` slab allocators, zero dynamic allocation on hot paths) |
| **Determinism & Jitter Guard** | Coarse scheduler ticks, manual application tuning | Dynamic thread priorities, low-latency interrupt paths | Sub-millisecond bounds, highly dependent on hardware configurations | Microsecond-bounded context-switches, high predictability | Extreme high determinism, minimal scheduling jitter | **Rate-Monotonic Scheduler** (256 priority levels, deterministic Open-Addressing Task Hash Table) |
| **Functional Safety (ISO 26262)** | None (Community-driven; commercial pre-certified forks exist) | Audited subset targeting SIL 3 / ASIL B profiles | Non-certifiable due to millions of lines of code | **Pre-certified ASIL D / SIL 4** (Commercial safety manual provided) | **Pre-certified ASIL D / SIL 4** (Commercial safety manual provided) | **Architected for ASIL D** (Traceability matrix, 100% state snapshot isolation between test runs) |
| **Application Loading Lifecycle** | Static Linking (Firmware image must be re-flashed) | Static Linking (DeviceTree and Kconfig frozen at compile-time) | Dynamic Loading (`execve` standard ELF, heavy runtime overhead) | Dynamic Loading (Isolated process management via process manager) | Dynamic Loading (Target execution of compiled RTP binaries) | **Dynamic `runelf` Subsystem** (Static ELF64 parsing from VFS/Initrd with execution quota validation) |
| **Idle Task Steward Infrastructure** | Minimal Hook (`configUSE_IDLE_HOOK` simple user callback) | Low-power state management and CPU power-down loops | Standard kernel idle loop, dynamic sleep ticks | System telemetry, background profiling counters | Performance monitoring and power management hooks | **ASIL D Integrity Monitors** (Slab page recovery, RAM March-C algorithm, CPU ALU test patterns) |
| **Integrated Test Coverage** | None (Relies on external test runner suites) | Integrated Twister framework for out-of-tree testing | Extensive LTP (Linux Test Project) suite, hosted execution | Proprietary commercial hardware validation tools | Proprietary commercial hardware validation tools | **665 In-Kernel Tests** (Automated boot-time verification with full leak detection) |

---

## Completed Roadmap Phases

- [x] **Phase 2 — Kernel Core & Shell** — Boot, PMM, VMM, scheduler, IPC, syscalls, ELF loader, shell, devfs, procfs
- [x] **Phase 3 — System Services & Hardware** — FAT32, tmpfs, PCI, Virtio, ATA PIO, RNG, FPU, DMA, network stack *(completed — v0.2.20)*
- [x] **Phase 4 — Kernel Configuration & Portability** — jarvis_config.h, CONFIG_* migration, check-config validation, multi-arch HAL infrastructure *(completed — v0.2.21)*
- [x] **Phase 5 — aarch64 Port (ARM Cortex-A)** — HAL refactoring, ARM boot, page tables, context switch, GICv3, timer, UART, PCI ECAM, Renode simulation *(completed — v0.2.22)*
- [x] **Phase 6 — riscv64 Port (RV64)** — RISC-V boot (OpenSBI→S-mode), Sv39 page tables, context switch, PLIC, SBI UART/timer, 3-arch build system, Renode support *(completed — v0.2.23)*
