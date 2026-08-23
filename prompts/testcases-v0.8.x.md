# Test Cases — v0.8.x (Phase 8: Microkernel — Kernel Reduction & Capabilities)

## Branch: testbed only

*Outline — test details to be expanded when implementation begins.*

### Kernel Reduction — test_kernel_core.cpp
- Kernel retains only: scheduler, IPC primitive (ports/capabilities), page-table management, interrupt routing to userspace
- Shell, init (PID 1), VFS, all drivers moved to ring-3 server processes
- Kernel shell removed — replaced by ring-3 userspace shell ELF loaded by `reboot_from_table()`
- Userspace shell uses syscalls for keyboard (`SYS_READ_TERMINAL`), framebuffer (`SYS_WRITE_TERMINAL`), serial I/O
- Userspace init spawns driver manager → device servers → VFS server → shell
- Kernel binary size reduced (measured against pre-microkernel baseline)

### Capability-Based Security — test_capability.cpp
- Each server holds capabilities for owned resources (IO ports, memory regions, IRQ lines)
- Kernel enforces capabilities on every IPC call
- No server can access another server's MMIO region or memory
- `SYS_CAP_GRANT` delegates capability to another task
- `SYS_CAP_REVOKE` revokes previously granted capability
- Revoked capability: subsequent access on that channel returns error
- Capability duplication: grant duplicates, revoke invalidates all copies
- Default-deny: task with no granted capabilities cannot access any protected resource
