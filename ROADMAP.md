# Jarvis RTOS — Development Roadmap

**Build:** v0.3.12-dev | **Last Release:** v0.3.11 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.3.11)

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.3.12

### Fine-Grained Lock & Safety-Guardrail Enforcement

Source: the three standing Safety & Concurrency Guardrails (§ above) are
**partially enforced** in the current tree.  This milestone audits each against
the live code, migrates every unjustified coarse-grain guard, and re-verifies
the guardrail tests.  All counts below are VERIFIED against the code
(2026-08-10).

#### G1 — Fine-Grained Locks: eliminate non-boot/panic/test `IrqGuard`

**Status:** 27 production `IrqGuard` sites remain outside boot/panic/test
isolation: `scheduler.cpp` (15), `ipc.cpp` (5), `task.cpp` (4),
`taskdefs.cpp` (2), `mutex.cpp` (1, comment-only).  The existing
`test_irqguard_audit.cpp` doc-block claims "Scheduler, IPC, and tmpfs have all
been migrated to SpinLock" — **stale** (scheduler has 15 IrqGuard sites).

- [ ] **G1-A — Classify every IrqGuard site.**  For each of the 27 sites,
      determine whether it is genuinely IRQ-exclusion (priority/effective-
      priority snapshot vs the timer ISR — e.g. ipc.cpp:421,469) or a coarse
      critical section that can be a `SpinLock`/`SpinLockGuard`/`sync::Mutex`.
      Output a site-by-site ledger (`docs/irqguard-ledger.md`) with
      justification per site.
- [ ] **G1-B — Migrate migratable sites to fine-grained locks.**  Replace
      every site classified as non-IRQ-exclusion with the sanctioned primitive
      (SpinLock for short sections, `sync::Mutex` for blocking paths).  Keep
      IrqGuard ONLY where the section must exclude the timer ISR (boot, panic,
      test isolation, and priority-snapshot consistency).
- [ ] **G1-C — Re-verify `test_irqguard_audit.cpp`.**  Update the stale
      doc-block to match the post-migration reality; the audit test must
      reflect the actual allowed site set (boot/panic/test + justified IRQ-
      exclusion).
- [ ] **G1-D — Regression gates.**  `scheduler`, `ipc`, `ipc_blocking`,
      `ipc_robustness`, `mutex_pcp`, `queue_pip`, `priority_inheritance`,
      `selftest`, `make build` (check-style Errors: 0).

#### G2 — Reference-Enforced Tasks: audit raw-pointer TCB/IPC manipulation

**Status:** scheduler/task/IPC manipulate TCB fields and IPC endpoints via raw
pointers (e.g. `scheduler.cpp:564,1288` direct field writes; `ipc.cpp` owner
priority snapshots).  Guardrail mandates references where the surrounding API
already does (e.g. `set_task_ready(*tcb)`, `BufferPool::transfer(*from, *to)`).

- [ ] **G2-A — Audit TCB endpoint APIs.**  Enumerate `Scheduler::` and `IPC::`
      functions taking `TaskControlBlock*`/raw handles where a reference is
      already the norm elsewhere; classify reference-migratable vs.
      null-check-required.
- [ ] **G2-B — Migrate reference-legal call sites.**  Convert to
      `TaskControlBlock&` where the caller provably holds a live object; keep
      raw pointers only where nullability is intrinsic (lookups, `find_task`).
- [ ] **G2-C — Regression gates.**  `scheduler`, `process`, `ipc`,
      `selftest`, `make build`.

#### G3 — Zero-Allocation tmpfs: verify node/data allocation discipline

**Status:** tmpfs nodes use `MemPool` (`tmpfs.cpp:190,195,292,297`) —
compliant.  File data allocates on demand via
`PMM::alloc_user_contiguous(16)` (`tmpfs.cpp:98`) — verify this is bounded and
tracked (ResourceTracker), not an unbounded heap path.

- [ ] **G3-A — Verify tmpfs node allocation is MemPool/BufferPool-only.**  Sweep
      `tmpfs.cpp` for any `new`/heap path; confirm all node lifetimes use
      MemPool alloc/free.
- [ ] **G3-B — Verify file-data page allocation is bounded + tracked.**  Confirm
      `PMM::alloc_user_contiguous` calls are accounted in ResourceTracker, freed
      on vnode cleanup, and cannot grow unboundedly (per-file page cap).
- [ ] **G3-C — Regression gates.**  `vfs`, `tmpfs` (if gated), `selftest`,
      `make build`.

#### G4 — Stale-doc cleanup (v0.3.10/0.3.11 residue)

- [ ] **G4-A — `docs/specs/oom-rt.md` §3.**  The "v0.3.12 open items" (A1-A4)
      are all landed in v0.3.10 (task.cpp:452, scheduler.cpp:563, vmm.cpp:465+
      null guards).  Rewrite §3 as "landed/closed" or repoint to ROADMAP_done.
- [ ] **G4-B — `testcases-v0.3.12.md`.**  Duplicates the v0.3.10 Alloc/Free
      audit (implemented + released).  Either delete (after summary update) or
      re-scope to the G1-G3 items above.
- [ ] **G4-C — `test_irqguard_audit.cpp` doc-block** (covered by G1-C).

**Required fix discipline (per AGENTS.md Mandatory Bugfix Sequence):**
1. Classify each: G1 = lock migration, G2 = reference migration, G3 = tmpfs
   allocation audit, G4 = doc cleanup (all concurrency/memory-safety, not
   timing).
2. Read the affected code + callers before editing (do not fix blind).
3. One hypothesis per item, validated by build + the smallest applicable test
   class.
4. Implement, `make build` clean, run the class to 0 failures.
5. After all items: `make execute-test x86_64 debug all` (835/835), then the
   release gate `make execute-test x86_64 release all` (84/84).

**Acceptance criteria (DONE when):**
- G1-A ledger written; G1-B/G1-C migrate + re-verify (27 IrqGuard sites
  reduced to the justified IRQ-exclusion set).
- G2-B reference-migration complete; no new raw-pointer TCB/IPC endpoints in
  the touched APIs.
- G3-A/G3-B verified bounded + tracked (no ResourceTracker delta in
  `vfs`/`tmpfs` classes).
- G4-A/G4-B/G4-C stale docs resolved.
- `make build` clean (check-style Errors: 0), `selftest` 132/132,
  `all` 835/835, release gate 84/84.
- `test-history.txt` rows appended for every class touched.

**Out of scope:** ~~H2 race (v0.3.9)~~ (RESOLVED), ~~BufferPool +1 (v0.3.11)~~
(RESOLVED), ISO 26262 certification artifacts, SMP (0.4.x), and userspace ABI
(0.5.x).

## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.3.11 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak).

---

## Future Roadmap (Aspirational)

### Phase 4.5: Memory Protection (0.4.x) — prerequisite for safe SMP
- [ ] **Requirement spec:** `docs/specs/memory.md` §7 (REQ-MP-01..06). Current state: user↔user isolation + user-stack guard pages present; kernel-task↔kernel-task isolation ABSENT; software canaries absent. Decisions: full private kernel page tables, both MMU guard pages + software canaries, HW enforcement (SMAP/SMEP/PAN/PXN) recommended-not-mandatory.
- [ ] **0.4.0-MP1** — Private kernel-half page tables per kernel task (clone kernel PML4, private data/bss/stack frames, CR3 switch; preserve HHDM for kernel→user access)
- [ ] **0.4.0-MP2** — MMU red-zone guard pages between text/data/heap/stack segments (kernel + user tasks)
- [ ] **0.4.0-MP3** — Software sentinel canaries at segment boundaries, verified on syscall + context-switch entry
- [ ] **0.4.0-MP4** — Optional HW enforcement: SMAP/SMEP (x86_64) / PAN/PXN (aarch64) with `stac/clac` audit
- [ ] **0.4.0-MP5** — Verification suite: cross-task #PF tests, canary-tamper detection, HHDM kernel→user read, SMAP/PAN negatives
- [ ] **0.4.0-MP6** — Kernel stack guard page via private VA window (moved from v0.3.7; requires snapshot-safe page table pool)
- [ ] **0.4.0-MP7** — `page_table_shared_` removal — complete deep-copy fork (walk all user entries, allocate new PDPT/PD/PT, copy contents). Current state: config + pool done.

### Phase 5: SMP + Multicore (0.4.x)
#### 0.4.1–0.4.2 — APIC & SMP Boot
- [ ] Local/IO APIC, X2APIC, per-CPU GDT/TSS, INIT-SIPI AP startup
- [ ] TPR-based interrupt prioritization, core state isolation
- [ ] **Per-CPU asm for `isr_nesting_depth`** — move from the single global symbol to GS-relative access on x86_64 (TPIDR/tp on aarch64/riscv64); the C++ side already uses `__atomic_*` (v0.3.7 PfA-B). CpuContext plumbing (`current_cpu()`) is in place.
- [ ] **`hhdm_modified_` (VAR-17) re-audit** — task-context only today (single-core safe); re-audit under SMP with per-CPU ownership or atomics.

#### 0.4.3–0.4.4 — Per-CPU Scheduling & Cache
- [ ] Distributed run queues, real-time load balancer, SYS_SET/GET_AFFINITY
- [ ] Cache coloring allocator, SMP spinlocks/rwlocks, WCET re-audit

#### 0.4.5–0.4.6 — TLB Shootdown & IPI Reduction
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
