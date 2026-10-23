# GDB Debugging Guide

## Quick Start

### Interactive Session

Build a steppable kernel, boot it to the shell, and freeze it on the
GDB stub — exact commands in `docs/clion-setup.md` §4.1–4.2
(`make debug NO_LTO=1`, `test-config.txt` to `none`, frozen QEMU with
`-s -S`). In a second terminal:

```bash
x86_64-elf-gdb build/kernel-debug.elf -x tools/gdb/init.gdb
```

The init script auto-connects to `:1234` and loads the kernel Python
plugin. Then `continue` to release the frozen CPU.

Scripted alternative (boots to shell and types a command file):

```bash
make debug-shell x86_64 debug none tools/gdb/init.gdb <shell-commands-file>
```

### Automated Panic Capture

```bash
make debug-test x86_64 debug <class> tools/gdb/test-batch.gdb
```

Builds the class, boots it under GDB surveillance with the stub on
`:1234`, and runs the batch script. If `panic()` fires, GDB captures
the full backtrace, registers, and task state
(`build/gdb-panic-captured` is touched, exit code 1); serial goes to
`build/gdb-serial.log`. Example: `class=testrunner`.

## Custom GDB Commands

| Command | Description |
|---------|-------------|
| `tasks` | List all scheduler tasks with PID, state, priority, parent |
| `task <n>` | Show detailed task info (by array index or PID) |
| `regs` | Print all GPRs, RFLAGS decode, and control registers |
| `pml4` | Walk the 4-level page table starting from CR3 |
| `pml4 -v` | Full verbose page table dump (all entries) |
| `panicinfo` | Dump current task, 32-frame backtrace, and registers |
| `step-into` | Single-step one instruction (`stepi`) |
| `step-over` | Step over function call (`next`) |
| `step-out` | Finish current function (`finish`) |
| `trace-syscall` | Set up syscall entry tracing breakpoint |

## Pretty-Printers

The following types auto-format when printed with `p`:

- `kernel::TaskControlBlock` → `TCB(pid=0x..., prio=..., state=...)`
- `kernel::TaskContext` → `TaskContext(rip=0x..., rsp=0x...)`
- `kernel::PhysicalAddress` → `PhysicalAddress(0x...)`
- `kernel::VirtualAddress` → `VirtualAddress(0x... [KERN/USER])`
- `kernel::PageAddress` → `PageAddress(0x...)`

## Common Workflows

### Debug a Kernel Panic

```bash
# Terminal 1: frozen QEMU as in Quick Start above
# Terminal 2:
x86_64-elf-gdb build/kernel-debug.elf -x tools/gdb/init.gdb
(gdb) continue
# ... wait for panic ...
# On stop, GDB hook-stop auto-prints RIP + backtrace
(gdb) panicinfo     # full dump
(gdb) tasks         # task state
(gdb) pml4          # page table
```

### Debug a Page Fault

```gdb
(gdb) break handle_interrupt_c if vector == 14
(gdb) continue
# On hit: CR2 has the fault address, error_code has type
(gdb) regs
(gdb) info registers cr2
(gdb) bt
```

### Debug a Specific Test

```gdb
(gdb) break test_task_lifecycle
(gdb) continue
(gdb) step-into
```

### Watch Syscall Activity

```gdb
(gdb) trace-syscall
(gdb) continue
# Each syscall prints its number
```

## Physical Memory Access

The kernel maps all physical memory at `0xFFFF800000000000 + phys_addr` (HHDM). To read physical address `0x1000`:

```gdb
(gdb) x/8gx 0xFFFF800000000000 + 0x1000
```

## Key Breakpoint Locations

| Location | File:Line | Purpose |
|----------|-----------|---------|
| `panic` | `kernel.cpp:1765` | Kernel panic (any reason) |
| `handle_interrupt_c` | `kernel.cpp:2300` | All exceptions + interrupts |
| `handle_interrupt_c if vector == 14` | — | Page fault only |
| `handle_interrupt_c if vector == 13` | — | General protection fault |
| `handle_interrupt_c if vector == 8` | — | Double fault |
| `kernel::Syscall::handle` | `syscall.cpp` | Any syscall entry |
| `kernel::Scheduler::reschedule` | `scheduler.cpp` | Context switch |
| `kernel::Scheduler::next_task` | `scheduler.cpp` | Task selection |

## Test Infrastructure

The test framework runs at boot. Tests register via `JARVIS_REGISTER_TEST(name)` macros. On failure:

```gdb
(gdb) break kernel::test::Registry::record_failure
(gdb) commands
  printf "TEST FAILED: file=%s line=%d expr=%s\n", file, line, expr
  bt
  continue
end
```
