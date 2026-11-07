# Syscall / VFS / ELF Boundary Specification

**Semantics:** binding trust-boundary contracts for Ring-3 → kernel entry, the
VFS/syscall layer, and the ELF loader.  Synthesis of the historical
boundary-audit implementation spec (12 confirmed audit defects; original
removed 2026-11 after extraction, see git history) and
`specs/privilege-audit.md` (ring-level audit).  `[IMPLEMENTED]` =
code-verified in the current tree.

```
 Ring-3 task
    │  int 0x80 (x86_64) / svc / ecall
    ▼
 isr_common ──▶ handle_interrupt_c ──▶ Syscall::handle
    │                                    │
    │                     boundary validation (CheckedPtr, once)
    │                                    │
    │   ┌────────────────────┬───────────┴──────────────┐
    │   ▼                    ▼                          ▼
    │  sys_fstat/ioctl     path syscalls             ELF loader
    │  (checked pointers)  (authorize→resolve→      (W^X, size-bound,
    │                      re-validate identity)     bounded argv/envp)
    │   ▼                    ▼                          ▼
    │  VFS core ──▶ fat32/tmpfs/devfs/initrd ──▶ drivers / vfsd
    └── Vnode refcount atomics, FdTable, cwd_lock_
```

## 1. Design Principles (binding)

1. **Validate at the boundary, once.** The syscall handler is the enforcement
   point for every Ring-3 pointer; wrap in `checked()`/`CheckedPtr<T>` and
   validate before any dereference (`CheckedPtr<uint8_t>` — `void` is not
   TriviallyCopyable).
2. **Resolve once, operate on the resolved object.** Path syscalls resolve the
   vnode + capture `ino`+fs-instance, authorize via `vfsd_authorize` IPC, then
   **re-resolve and compare identity** before operating (TOCTOU closure).
3. **Every refcount mutation is atomic.** Use the returned previous value of
   the atomic decrement for the zero-check — never re-read.
4. **W^X in the PTE.** Segment permissions derive from `phdr->flags`; user
   stack/heap writable-only with NX (bit 63) set when not executable.
5. **Every blocking primitive is bounded or deschedules.** No `UINT64_MAX`
   pause-spin; `sys_receive` has a timeout variant.
6. **Freestanding constraint.** `<atomic>` unavailable (`-nostdinc`); use GCC/
   Clang builtins (`__atomic_fetch_add/sub/load/store`) — same lock-free
   semantics as `std::atomic<int>`.

## 2. VULN Status Ledger (boundary audit, 12/21 confirmed)

| ID | Finding | Fix | Status |
|---|---|---|---|
| VULN-C1 | `sys_fstat` raw Ring-3 pointer | `checked()` reject with -1 | IMPLEMENTED |
| VULN-C2 | `sys_ioctl` unchecked pointer to driver | `checked(arg2, sizeof(uint64_t))`; VnodeOps::ioctl takes CheckedPtr | IMPLEMENTED |
| VULN-C4 | authorize-then-resolve TOCTOU | resolve-first + identity re-check (capture ino+fs) | IMPLEMENTED |
| VULN-C5/C6 | unsync `Vnode::refcount` + FdTable | `std::atomic<int>`-style builtins; `cwd_lock_` for sys_chdir | IMPLEMENTED |
| VULN-H1 | uniform page perms defeat W^X | permission bitmask in map_page_in_pml4; per-segment W/X from phdr; stack/heap RW-only; NX | IMPLEMENTED |
| VULN-H2 | OOB ELF read `phdr->offset+filesz` | thread real file size; `offset+filesz > file_size` check | IMPLEMENTED |
| VULN-H4/W1 | unbounded argv/envp scan | cap `MAX_EXEC_ARGS`/`MAX_EXEC_ARG_LEN`; validate window before scanning | IMPLEMENTED |
| VULN-U2 | `setup_user_stack` unbounded underflow | hard reservation `str_total + kStackReserve < STACK_SIZE` | IMPLEMENTED |
| VULN-W2 | unbounded busy-wait tty/kbd | BLOCKED + reschedule pattern | IMPLEMENTED |
| VULN-W3 | `sys_receive` no timeout | `arg3` = `timeout_ticks` (0 = forever); deadline in loop | spec, verify pending |

Regression gate (passed 2026-08-02): syscall 19/19, process 43/43, vfs
146/146, security 31/31, `all` 881/881.

## 3. Privilege / Ring-Level Audit (v0.2.9)

**Ring-0 mandatory:** GDT/IDT/ISR stubs, PMM/VMM, scheduler, syscall entry
(`syscall`/`sysret`, `swapgs`, STAR/LSTAR/FMASK).
**Migration candidates** (user-space servers; vfsd/iocd are now kernel
daemons): VFS core / initrd_fs / procfs → `/sbin/vfsd`; Keyboard / Serial /
Timer-RTC → `/sbin/iocd`; Shell is already userspace `sh.c`.
**Hard rule:** a user task must never read/write any kernel text/data/stack.

## 4. Gaps

- **VFS design spec [RESOLVED]:** created `specs/vfs.md` (vnode model, mount
  model, vfsd protocol, path resolution + TOCTOU closure, FdTable, backends).
- **`sys_receive` timeout (VULN-W3)** verification status unconfirmed.
- **Syscall ABI header** (`src/kernel/syscall/syscall.h` with documented trap
  vectors/IRQ numbers + register conventions) — deferred to Phase 6 (0.5.x,
  ROADMAP).
