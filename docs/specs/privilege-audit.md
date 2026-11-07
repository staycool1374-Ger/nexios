# Kernel Subsystem Privilege Audit

**Status:** live reference — statuses refreshed 2026-11; supersedes
`docs/_archive/privilege_audit.md` (v0.2.9, removed 2026-11; original text
preserved in git history).
**Scope:** per-subsystem Ring / HW-access inventory + Ring-3 migration
candidacy for the microkernel transition.
**Related:** `specs/microkernel-transition.md` (live migration plan),
`specs/boundary.md` (trust-boundary contracts). Migration work tracked as
GH issues #47 (VFS/block), #48, #51 (serial/console).

## Legend

- **Ring**: current privilege level (0 = kernel, 3 = user).
- **HW Access**: hardware resources used.
- **Can move to R3?**: whether the subsystem could run in userspace.
- **Status 2026-11**: verified current state (tree + issues).

## 1. Architecture (arch/)

| Component | Ring | HW Access | R3? | Status 2026-11 |
|---|---|---|---|---|
| GDT | 0 | `lgdt`, MSRs (TSS, FS/GS) | NO | stays ring-0 |
| IDT | 0 | `lidt` | NO | stays ring-0 |
| ISR stubs | 0 | exception vectors, IRQ gates | NO | stays ring-0 |
| Timer/PIT | 0 | ports 0x40-0x43, IRQ0 | handler stays R0; read-only via syscall | unchanged |
| RTC | 0 | ports 0x70-0x71, IRQ8 | handler stays R0; read-only via syscall | unchanged |
| Keyboard | 0 + PD | ports 0x60/0x64, IRQ1 | **iocd/PD** | `userspace/kbd-pd.c` scaffold (48 lines); kernel driver retained |
| Serial | 0 + PD | ports 0x3F8-0x3FF | **iocd/PD** | `userspace/serial-pd.c` drains SHM SPSC ring (#51 step-c-prime, on main); kernel driver retained |
| MSR | 0 | `rdmsr`/`wrmsr` | NO | stays ring-0 |
| I/O wrappers | 0 | `in`/`out`, CRx, `hlt`, `cli`/`sti`, `rdtsc` | NO | stays ring-0 |

## 2. Memory Management (memory/)

| Component | Ring | HW Access | R3? | Status 2026-11 |
|---|---|---|---|---|
| PMM | 0 | EFI map, phys pages | NO | stays ring-0 |
| VMM | 0 | CR3, INVLPG, HHDM | NO | stays ring-0 |
| MemPool | 0 | none (PMM pages) | NO (libc heap covers userspace) | stays ring-0 |
| address.hpp | 0 | none | shareable utility | unchanged |
| CheckedPtr | 0 | none | kernel-only (boundary enforcement) | unchanged |

## 3. Task Management & Scheduling (task/)

| Component | Ring | HW Access | R3? | Status 2026-11 |
|---|---|---|---|---|
| TaskControlBlock | 0 | PMM stacks, VMM clone | NO | stays ring-0 |
| Scheduler | 0 | CR3/RSP0 via TSS, ticks | NO | stays ring-0 |

## 4. IPC (ipc/)

MessageQueue/IPC: ring 0. `Message`/`MessageQueue` shapes could be shared,
but `send`/`recv` mutate task state → stays ring-0. Unchanged.

## 5. Synchronization (sync/)

Notify/EventGroup/Mutex/Semaphore/Queue: ring 0, exposed to userspace via
syscalls where applicable. Unchanged.

## 6. Virtual File System (vfs/)

| Component | Ring | HW Access | R3? | Status 2026-11 |
|---|---|---|---|---|
| VFS core | 0 | none (Vnodes, mounts, resolve) | **YES → vfsd** | #47 step-a merged (`d1b44d849`): kernel `vfs_srv` + `userspace/vfsd.c` shadow (strangler, no flip); 25 files still in-kernel |
| initrd_fs | 0 | initrd phys buffer | partial (needs `map_physical`-style mapping) | unchanged |
| devfs | 0 | COM1, kbd ring | **split** (dirs → vfsd, device I/O → iocd) | unchanged |
| procfs | 0 | Scheduler/PMM stats | **YES → vfsd** (needs stats syscall) | unchanged |
| pipe | 0 | none (buffer + semaphore) | partial (ring + notify syscalls) | unchanged |

## 7. ELF Loader (elf/)

Ring 0 (maps pages into target AS). Stays ring-0. Unchanged.

## 8. Syscall Layer (syscall/)

Entry (`syscall_entry.asm`, `swapgs`/STAR/LSTAR) + dispatch/handlers:
ring 0, fundamentally. Unchanged. (Userspace path uses `int $0x80` via
`src/libc/syscall.h` → `isr_128`.)

## 9. Services (services/)

| Component | Ring | HW Access | R3? | Status 2026-11 |
|---|---|---|---|---|
| Shell | 0 + R3 | kbd/serial/Terminal | **YES** | `userspace/sh.c` (442 lines) runs in R3; kernel `src/services/shell.cpp` retained |
| Terminal/framebuffer | 0 + PD | fb MMIO, serial | partial | `userspace/fb-pd.c` scaffold (48 lines); `src/services/terminal/` still kernel (#51 full acceptance open) |
| Program registry | 0 | none | YES (userspace ELFs) | unchanged |

## 10. Drivers (driver/)

| Component | Ring | HW Access | R3? | Status 2026-11 |
|---|---|---|---|---|
| Driver registry | 0 | none | YES (tracking service) | unchanged |
| Block (AtaPio/Ahci/VirtioBlk) | 0 + PD | IRQ/DMA/MMIO | **YES → block PD** | #47 step-a: `block_srv` + `userspace/block-srv.c` + parity (no flip); `userspace/block-pd.c` scaffold; IRQ/DMA still kernel |
| Net | 0 + PD | NIC MMIO/IRQ | **YES → net PD** | `userspace/net-pd.c` present; driver still kernel |

## 11. Libraries (lib/)

Logger/Version/Test-framework: ring 0, mostly already syscall-exposed
(`write`, `sys_uname`). Unchanged.

## Migration order (live)

1. ✅ IPC baseline + SHM ring transport (#51 step-c-prime, on main).
2. Unprivileged shell (exists as `userspace/sh.c`; auto-start open).
3. Capability-based HW access (cap-broker; issue #240).
4. User-space VFS server (issue #47; step-a strangler landed, flip open).
5. User-space driver servers (issues #47/#48; IRQ + I/O-port + IOMMU delegation).
