# Jarvis RTOS — Development Roadmap

**Build:** v0.4.0-dev | **Last Release:** v0.3.12 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.3.12)

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.4.0

### Memory Protection (Phase 4.5) — prerequisite for safe SMP

Source: `docs/specs/memory.md` §7 (REQ-MP-01..06). Current state:
user↔user isolation + user-stack guard pages present; kernel-task↔kernel-task
isolation ABSENT; software canaries absent. Decisions: full private kernel
page tables, both MMU guard pages + software canaries, HW enforcement
(SMAP/SMEP/PAN/PXN) recommended-not-mandatory.

- [ ] **MP-1 — Private kernel-half page tables per kernel task** (clone kernel
      PML4, private data/bss/stack frames, CR3 switch; preserve HHDM for
      kernel→user access)
- [ ] **MP-2 — MMU red-zone guard pages** between text/data/heap/stack segments
      (kernel + user tasks)
- [ ] **MP-3 — Software sentinel canaries** at segment boundaries, verified on
      syscall + context-switch entry
- [ ] **MP-4 — Optional HW enforcement:** SMAP/SMEP (x86_64) / PAN/PXN
      (aarch64) with `stac/clac` audit
- [ ] **MP-5 — Verification suite:** cross-task #PF tests, canary-tamper
      detection, HHDM kernel→user read, SMAP/PAN negatives
- [ ] **MP-6 — Kernel stack guard page** via private VA window (moved from
      v0.3.7; requires snapshot-safe page table pool)
- [ ] **MP-7 — `page_table_shared_` removal** — complete deep-copy fork (walk
      all user entries, allocate new PDPT/PD/PT, copy contents).  Current
      state: config + pool done.

**Required fix discipline (per AGENTS.md Mandatory Bugfix Sequence):**
1. Classify each: MP-1/MP-2/MP-6/MP-7 = memory/page-table, MP-3 = canary,
   MP-4 = HW enforcement, MP-5 = verification suite.
2. Read the affected memory/VMM/task code before editing.
3. One hypothesis per item, validated by build + the smallest applicable test
   class (MP-1/MP-7 → `pml4_clone`/`process`; MP-5 → `memory_safety`/`pmm`).
4. Implement, `make build` clean, run the class to 0 failures.
5. After all items: `make execute-test x86_64 debug all` (835/835), then the
   release gate `make execute-test x86_64 release all` (84/84).

**Acceptance criteria (DONE when):**
- MP-1..MP-7 fixed (each verified by build + class gate).
- `page_table_shared_` fully removed (MP-7); every kernel task has a private
  kernel-half page table (MP-1).
- Guard pages + canaries active; SMAP/SMEP enforced on x86_64 (MP-2/3/4).
- MP-5 verification suite passes; `make build` clean (check-style Errors: 0),
  `selftest` 132/132, `all` 835/835, release 84/84.
- `test-history.txt` rows appended for every class touched.

**Out of scope:** ~~H2 race (v0.3.9)~~ (RESOLVED), ~~BufferPool +1 (v0.3.11)~~
(RESOLVED), ~~IrqGuard guardrails (v0.3.12)~~ (RESOLVED), SMP (0.4.x APIC
boot), microkernel capability foundation (0.4.x), and userspace ABI (0.5.x).

## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.3.11 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak).

---

## Future Roadmap (Aspirational)

### Phase 4.6: Microkernel Primitives & Capability Foundation (0.4.x) — prerequisite for 0.7.x Microkernel Transition

#### 0.4.1 — Capability-Based Access Control Architecture (CSpace)
- [ ] **Capability System Specification** — Design object capability structures (CNode, CSlot, CSpace) for tasks, IPC endpoints, physical memory frames, Untyped memory, IRQ controls, and MMIO ranges (eliminating ambient authority).
- [ ] **Capability Lifecycle Primitives** — Implement `SYS_CAP_GRANT`, `SYS_CAP_REVOKE`, `SYS_CAP_MINT`, and `SYS_CAP_COPY` system calls with deterministic reference-counted cleanup.
- [ ] **Untyped Memory Allocator (seL4-style)** — Transition physical allocation away from direct PMM calls to explicit Untyped memory retyping (`Untyped.Retype` into Frame, PageTable, CNode, or Endpoint capabilities).

#### 0.4.2 — User-Space Driver Infrastructure & Hardware Isolation
- [ ] **User-Space IRQ Delivery System** — Hardware IRQ dispatcher that transforms incoming interrupts into Capability-backed IPC Notifications (`sys_irq_register` / `sys_irq_wait`) to eliminate Ring 0 driver execution.
- [ ] **Fine-Grained Hardware I/O Delegation** — Per-task TSS I/O port bitmap management (`sys_ioport_grant`) and capability-gated MMIO page frame mapping for Ring 3 drivers.
- [ ] **IOMMU DMA Protection Layer (VT-d / AMD-Vi / SMMU)** — Program IOMMU translation tables to isolate Ring 3 driver DMA requests strictly to task-owned physical memory frames.

#### 0.4.3 — High-Performance Zero-Copy IPC & Fault Isolation
- [ ] **In-Register IPC Fastpath** — Assembly fastpath for short messages passed via CPU registers (`rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`) skipping full TCB context saves.
- [ ] **External Pager Protocol (#PF Delegation)** — Exception forwarding mechanism routing user-space `#PF` / page faults over IPC to a capability-designated Ring 3 Pager thread.
- [ ] **Fault Recovery & Crash Supervisor** — Asynchronous task-death notifications allowing supervisor processes to catch server crashes, reclaim leaked capabilities, and restart failed drivers cleanly.
- [ ] **Capability Shared-Memory Granules** — Zero-copy ring buffer shared memory mappings (`SYS_SHM_MAP`) backed by capabilities for high-throughput client-server I/O.

### Phase 5: SMP + Multicore (0.4.x)
#### 0.4.4 — APIC & SMP Boot
- [ ] Local/IO APIC, X2APIC, per-CPU GDT/TSS, INIT-SIPI AP startup
- [ ] TPR-based interrupt prioritization, core state isolation
- [ ] **Per-CPU asm for `isr_nesting_depth`** — move from the single global symbol to GS-relative access on x86_64 (TPIDR/tp on aarch64/riscv64); the C++ side already uses `__atomic_*` (v0.3.7 PfA-B). CpuContext plumbing (`current_cpu()`) is in place.
- [ ] **`hhdm_modified_` (VAR-17) re-audit** — task-context only today (single-core safe); re-audit under SMP with per-CPU ownership or atomics.

#### 0.4.5— Per-CPU Scheduling & Cache
- [ ] Distributed run queues, real-time load balancer, SYS_SET/GET_AFFINITY
- [ ] Cache coloring allocator, SMP spinlocks/rwlocks, WCET re-audit

#### 0.4.
6 — TLB Shootdown & IPI Reduction
- [ ] PCID, selective INVPCID, lazy shootdowns, IPI batching, latency profiling

### Phase 6: System Integration / Userspace ABI (0.5.x)

**Priority:** picolibc integration — syscall ABI, TLS, POSIX stubs.

#### Syscall ABI Definition
- [ ] **Document trap/IRQ numbers** — create `src/kernel/syscall/syscall.h` with stable, documented trap vectors and IRQ numbers
- [ ] **Register conventions** — specify register layout for syscall arguments and return values per architecture (x86_64: `rax=num, rdi, rsi, rdx, r10, r8, r9`; aarch64: `x8=num, x0-x5`; riscv64: `a7=num, a0-a5`)
- [ ] **syscall.h public header** — export to userspace, used by both kernel dispatcher and libc stubs

#### picolibc Integration
- [ ] **POSIX syscall stubs** — implement `src/libc/picolib_stubs.c` with wrappers for `_write`, `_read`, `_sbrk`, `_exit`, `_open`, `_close`, `_fstat`, `_lseek`, `_getpid`, `_kill` using `jarvis_syscall()` dispatcher
- [ ] **Build picolibc** — compile with meson as `libc.a` + `libm.a` (static), targeting x86_64-elf
- [ ] **Makefile integration** — link `libc.a`/`libm.a` into kernel image; add build rules for picolibc subproject
- [ ] **TLS on context switch** — every task switch must load the thread-local-storage address into the appropriate base register (`FS` on x86_64, `TPIDR_EL0` on aarch64, `tp` on riscv64). picolibc uses this for `errno` and per-task internal state — no global locks needed.
- [ ] **Verify** — `printf`, `malloc`, `scanf` work from userspace tasks via syscall stubs

### Phase 7: Safety Systems (0.6.x)
- [ ] ICH9/HPET hardware watchdog + NMI pre-timeout, PIT fallback, SYS_WATCHDOG_KICK
- [ ] Per-task software watchdog (SYS_WATCHDOG_CREATE), /proc/[pid]/watchdog
- [ ] Wait-for-graph deadlock detection, watchdog-driven recovery, SYS_HEALTH_STATUS
- [ ] Idle-task safety monitors: RAM March C-, CPU ALU verification, utilisation tracking

### Phase 8: Microkernel Transition (0.7.x–0.8.x)
- [ ] Externalise VFS & block I/O to user-space servers
- [ ] Externalise device drivers (keyboard, framebuffer, timer/RTC)
- [ ] Kernel reduction: scheduler, IPC, page-table management, interrupt routing only
- [ ] Capability-based security (SYS_CAP_GRANT / SYS_CAP_REVOKE)

### Phase 9: Hardware Drivers & Protocols (0.9.x)
- [ ] Full TCP/IP stack (ARP, IP, ICMP, UDP, TCP) with Ethernet NIC driver
- [ ] USB driver stack (UHCI/EHCI/xHCI)
- [ ] Hot-path secure call sequence layer (<seqguard.hpp>)
