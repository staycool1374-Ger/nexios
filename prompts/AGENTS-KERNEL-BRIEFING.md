# Jarvis RTOS — Kernel Architecture Briefing

## Overall Target
A rock-steady freestanding C++20 RTOS. User loads ELF via `runelf` as a strictly
periodic user task under full OS control, with access to all resources (vfat, net,
IPC). Every new task is checked against the Liu-Leyland bound *before* activation
to guarantee schedulability and time constraints. The idle task additionally
verifies OS and user ELF authenticity and runs diagnostic time measurements.

---

## 1. Scheduler — O(1) Bitmap + RMS
- 128-level ReadyQueueManager (`PriorityMap` hi/lo bitmaps + `TaskQueue[]` per level)
- O(1) enqueue/dequeue via bitmap scan (`find_highest_bit`)
- `effective_priority()`: uses `sporadic_server->current_priority()` if server exists, else raw TCB priority
- Task states: IDLE → READY → RUNNING → BLOCKED → TERMINATED
- RUNNING tasks are NOT in the ready queue (`next_task()` only picks READY; RUNNING is skipped)
- `current_task_ptr_` points to the currently dispatched task (updated by ISR epilogue via `scheduler_on_context_switch`)
- Task registry: `AllTasksRegistry` with per-priority intrusive linked lists (`pri_next_`/`pri_prev_`, buckets indexed by `all_bucket_`)

## 2. Deferred Context Switch (CRITICAL GOTCHA)
- `switch_to_task()` atomically publishes three globals:
  - `scheduler_save_rsp_to` = &current->context.rsp (address of current's RSP field)
  - `scheduler_load_rsp_from` = next->context.rsp (value of next's RSP)
  - `scheduler_next_task_id` = next->id
  - `scheduler_load_cr3_from` = next->page_table_ (if userspace task)
- Function returns normally — calling task continues on CPU even if state=BLOCKED
- Actual register switch: performed by ISR assembly (`isr_common`) AFTER `handle_interrupt_c` returns
- **Problem:** `rate_monotonic_schedule()` in the timer ISR calls `switch_to_task()` which OVERWRITES the globals, losing the pending switch. The intended target task is left RUNNING but never on CPU.
- **Fix:** RMS must check `__atomic_load_n(&scheduler_save_rsp_to) != 0` → return early

## 3. Voluntary (reschedule) vs Preemptive (RMS)
- `reschedule()`: called from `sys_receive` (queue empty) or any blocking operation.
  Sets deferred globals → returns → caller continues → `hlt()` → next timer ISR performs switch.
  Uses `arch::IrqGuard` (NOT scheduler_lock_) to avoid blocking the timer ISR's `rate_monotonic_schedule()`.
- `rate_monotonic_schedule()`: called from `on_tick()` in timer ISR context.
  Tries `scheduler_lock_.try_lock()` — if lock is held (test task inside a guarded section), returns early.
  On success: clears pending switch, calls `next_task()`, calls `switch_to_task()` if higher-priority task ready.
  **RMS must NOT override pending voluntary switches.** Check `scheduler_save_rsp_to` first.

## 4. Sporadic Server
- vfsd: `init(2, 10, 0)` — budget=2 ticks, period=10 ticks, bg_priority=0
- iocd: `init(3, 10, 0)` — budget=3 ticks, period=10 ticks, bg_priority=0
- `on_completion()`: schedules replenishment at `activation_time + period_t`, state → IDLE
- `process_replenishments()`: on every timer tick, fires due replenishments, restores budget
- **Does NOT call `set_task_ready()`** — only manages budget, never scheduler task state
- `current_priority()` returns bg_priority when EXHAUSTED, base_priority otherwise
- `consume()`: decrements `budget_remaining_`, transitions to EXHAUSTED at 0

## 5. IPC — Producer/Consumer Daemons
- vfsd (PRI 20, SS(2,10,0)): VFS operations. Blocks in `sys_receive` until IPC message arrives.
- iocd (PRI 20, SS(3,10,0)): I/O device handler. Producer — makes VFS syscalls → sends IPC to vfsd.
- `IPC::send(dest_id, msg)`: pushes to dest's msg_queue, calls `set_task_ready(dest)` if BLOCKED
- `IPC::recv(msg)`: pops from own msg_queue. Returns false if empty.
- `IPC::send_sync(dest_id, msg, reply)`: send + block-wait-for-reply (used by VFS syscall handlers)
- `MessageQueue` per task (allocated in `init_task_common`, freed in `cleanup`). Circular buffer, SpinLock-guarded.
- **Each task has its own msg_queue** — vfsd and iocd receive on their own queues.

## 6. Build System
| Target | Description |
|--------|-------------|
| `make debug` | Debug build (CONFIG_DEBUG, -Og), `debug/jarvis-rtos.iso` |
| `make release` | Release build (-O2, no CONFIG_DEBUG), `release/jarvis-rtos.iso` (x86_64 only) |
| `make execute-test <arch> <build> <class>` | Unified test runner (replaces all old test targets) |
| `make debug-test <arch> <build> <class> <gdb-script>` | GDB batch surveillance with panic capture |
| `make debug-shell <arch> <build> none <gdb-script> <shell-cmds>` | GDB + serial interaction from commands file |
| `make run-debug-mode [<arch>]` | Interactive debug QEMU (alias for execute-test debug none) |
| `make run-release-mode [<arch>]` | Interactive release QEMU (alias for execute-test release none) |

**Parameters:**
- `<arch>` — `x86`|`x86_64`|`arm`|`aarch64`|`riscv`|`riscv64` (shorthand accepted, default: x86_64)
- `<build>` — `debug`|`release`
- `<class>` — `none` (interactive shell) | `selftest` (CI gate) | `all` (full suite) | `<name>` (specific class)

**Key behaviours:**
- Cross-arch: `run-release-mode arm` builds aarch64 release ELF and boots via `qemu-system-aarch64 -kernel`
- x86_64 release: builds full GRUB ISO; non-x86 release builds ELF/binary booted via `-kernel`
- Incremental: x86_64 release interactive reuses existing ISO if present (skip rebuild)
- Arch switching via `make run-*-mode <new-arch>` triggers `make clean` automatically via `check-arch` stamp
- Positional arguments consumed by match-all `%:: @true` at end of Makefile

**Examples:**
```
make run-debug-mode                       # interactive x86_64 debug
make run-release-mode                     # interactive x86_64 release
make run-release-mode arm                 # interactive aarch64 release
make run-release-mode riscv               # interactive riscv64 release
make execute-test x86 debug all           # full debug suite
make execute-test x86 release selftest    # CI gate
```

- CXXFLAGS: `-std=c++20 -ffreestanding -fno-exceptions -fno-rtti -mno-red-zone -mcmodel=large`
- x86_64 GDB commands: `tasks`, `task <n>`, `regs`, `pml4`, `panicinfo`

## 7. Framebuffer / Terminal
- Physical address from multiboot2 framebuffer tag, mapped to HHDM_OFFSET + phys_addr
- Memory type: UC (Uncacheable) — each byte write goes to PCI bus (extremely slow)
- Typical: 1024×768, pitch 4096, bpp 32, fb phys at ~0x80000000 (PCI MMIO hole)
- `Terminal::clear()` → `memset(fb, 0, pitch * text_h)` — ~3MB UC write
- `Terminal::scroll()` → `memmove(fb, fb + row_size, ...)` — ~2.9MB UC read+write
- `Terminal::draw_char()` → pixel-by-pixel via `put_pixel` — 16×8 glyph = 128 pixel writes
- `Terminal::set_fb_enabled(true/false)` gates all framebuffer rendering
- `Terminal::write()` → `serial_putchar()` FIRST, THEN framebuffer rendering
- STATUS_BAR_ROWS=2, `rows_ = (height / FONT_HEIGHT) - 2`
- **Avoid pixel-by-pixel framebuffer loops in performance-critical paths**

## 8. Task TCB (TaskControlBlock)
- Fields: id, parent_id, state, priority, base_priority, period_ticks, deadline_ticks, remaining_ticks
- `kernel_stack` / `kernel_stack_top` / `stack_phys_` — kernel-mode stack (via kslot window in production, HHDM under test)
- `kstack_slot_va_` / `kstack_slot_size_` — kslot window slot (guard page at base, kernel stack above)
- `page_table_` — userspace page table (0 = kernel task, runs in kernel PML4)
- `all_bucket_` — AllTasksRegistry priority bucket index (SCHED-005, O(1) removal)
- `generation` — monotonically-increasing counter for stale-TCB detection in waiter arrays (SYNC-03)
- `msg_queue`, `notify`, `event_group`, `sporadic_server` — per-task resources
- `runq_next_` / `runq_prev_` — intrusive linked-list for O(1) ready queue
- `rq_priority_` — priority bucket in ReadyQueueManager
- `in_ready_queue_` — guard against double-enqueue
- `signal_handlers[MAX_SIGNAL_HANDLERS]` — per-task signal dispatch (init to nullptr)
- `debug_switch_ring[4]` — circular buffer of last 4 context switches (CONFIG_DEBUG only)
- `cwd` / `cwd_vnode` / `fd_table` — per-task filesystem state
- `program_break` / `program_break_start` — heap management

## 9. Context Switch Assembly (isr_stubs.asm, x86_64)
- All 256 ISRs push vector (+ dummy error for no-error vectors) → jmp `isr_common`
- `isr_common`: inc `isr_nesting_depth` → push r15–rax → call `handle_interrupt_c`
- After C handler: cli → load `scheduler_save_rsp_to` → if non-null && nesting==1:
  - `mov [save_to], rsp` (save current RSP to current->context.rsp)
  - `mov rsp, [load_from]` (load next task's RSP)
  - clear `scheduler_save_rsp_to`
  - call `scheduler_on_context_switch` → updates `current_index_` via `scheduler_next_task_id`
  - optional CR3 load (userspace task switch)
- `.restore`: pop rax–r15 → `add rsp, 16` (pop vector+error) → dec nesting → `iretq`

## 10. Kernel Boot Sequence
- `arch_init()` → `memory_init()` → `kernel_init()`
- `kernel_init`: `BufferPool::init()` → `daemon::init()` → `Shell::init()`
- → `reboot_from_table()` (spawns init, vfsd, iocd, user-app from `g_task_defs`
  — the single source of truth for priorities/periods) → idle loop
- Boot task set: init (PID 1, prio 0 background / 10 harness-in-testmode, period
  100), vfsd (prio 20, SS(2,10,0)), iocd (prio 20, SS(3,10,0)), user-app
  (prio 2, aperiodic)
- `init_task_main`: mounts fstab → runs /etc/rc (user-elfs @ prio 2, aperiodic)
  → waits for daemon-ready → runs test suite (raises self to prio 10) → creates
  ksh (prio 2, aperiodic) → reap loop (drops self to prio 0)
- Idle task: `sti` → `1: hlt` → `jmp 1b`. Timer ISR wakes it → RMS picks highest-priority READY task.

## 11. Shell Task (ksh, PRI 2, aperiodic)
- `shell_task_main()`: `init()` → write banner → `while(true) { prompt; readline; parse_and_exec; }`
- prompt: `✓ <cwd> $ ` (UTF-8 checkmark, directory, dollar)
- `readline`: polls COM1 LSR for serial input, `Keyboard::getchar` for PS/2, `arch::pause()` yield
- `init()`: registers built-in commands (help, echo, uptime, clear, pwd, etc.)
- `parse_and_exec()`: dispatches command by name. Add `runelf` for ELF loading.

## 12. Key Gotchas
1. **Deferred switch override** → RMS must NOT override pending voluntary switches
2. **O(1) queue only sees READY** → RUNNING tasks are invisible after lost switch
3. **Sporadic server never wakes tasks** → manages budget only, not scheduler state
4. **Framebuffer is UC MMIO** → pixel loops are extremely slow, avoid in hot paths
5. **`serial_putchar()` is busy-wait** → each byte polls UART LSR bit 5
6. **MemPool doesn't zero** → all allocs must be explicitly initialized (memset)
7. **`Logger::raw_write()` bypasses Terminal** → writes directly to serial UART
8. **`Terminal::write()` → serial FIRST, framebuffer second** → serial always gets output
9. **IPC debug prints behind CONFIG_DEBUG** → silent in release builds
10. **SCHED/RSCHED prints behind CONFIG_DEBUG** → silent in release builds
11. **Liu-Leyland bound warnings are informational** → not enforced by scheduler yet
12. **`sti(); hlt(); cli();` in `sys_receive`** → workaround for deferred switch; may be removable after RMS fix
13. **PMM ownership is alloc-time only** — `free_page()` does NOT modify the owner bit (VULN-004). Freed pages retain their USER/KERNEL owner bit. `free_user_pages()` MUST clear parent page-table entries after freeing child pages to prevent re-walk into reused memory.
14. **kslot vs HHDM stacks** — Kernel tasks in test mode use HHDM direct mapping (no guard page). Production kernel tasks and all user tasks use the kslot window (guard page below stack). The dual-path is ASIL-D-certified safe: test isolation provides equivalent fault containment via full memory rewind.
15. **O(1) free list covers HHDM only** — The PMM free list (VULN-003) only includes pages within the 128MB HHDM window. Pages beyond 128MB are inaccessible via the direct map and are never added to the free list. The bitmap scan fallback is also limited to the HHDM window.
16. **RSP-owner resolution in switch_to_task** — The O(n) scan that resolves the physically-running task by kernel-stack ownership is retained in debug builds and for release-build drift detection. It cannot be removed while mixed kslot/HHDM stacks exist (SCHED-008 was reverted for this reason).

## 13. Enforcement Rules (MANDATORY — violations halt and require human input)

### Rule A: Hypothesis-First Checkpoint
Before ANY code edit, output `[HYPOTHESIS] <specific root cause>` and `[EVIDENCE] <plan to prove/disprove>`. Wait for user confirmation before touching any file. No exceptions. This applies even to "obvious" one-line fixes.

### Rule B: No "Pre-Existing" Dismissal
Never claim a test failure is "pre-existing" or "not caused by my changes." Every failure must be investigated and fixed. The only exception is a test that was documented as failing in ROADMAP.md or BUGS.md BEFORE the current session started, AND you have run it on the baseline commit and confirmed the same failure signature.

### Rule C: No Push Suggestions
Never suggest, ask about, or initiate pushing commits. Only push when explicitly told to.

### Known Trap: `restore_task_fields` Skips TCBs with Bad Magic

A common test-isolation failure: `remove_task` logs `magic=0xDDDDDDDDDDDDDDDD`
with `[CLEANUP] skip poisoned TCB`. The chain:

1. Test frees a TCB → `MemPool::free` fills block with `0xDD` (CONFIG_DEBUG)
2. `snapshot_restore` restores MemPool bitmap → block allocated again but
   content is still `0xDD`
3. `restore_task_fields` (`scheduler.cpp:2129`) checks `t->magic`:
   ```cpp
   if (t->magic != TaskControlBlock::TCB_MAGIC)
       continue;  // skips restore, leaves 0xDD
   ```
4. TCB stays in `all_tasks_` with `magic=0xDD` → `remove_task` detects it
   on the next cycle.

**Fix:** Restore TCB fields regardless of magic. The MemPool bitmap restore
guarantees the block belongs to the TCB. Confirmed via lldb breakpoint at
`0xFFFF80000024E1B6` (the `jne` skip) — see `docs/investigation-cumulative-corruption.md`
Attempt 9 for details.

### Rule D: Debugging Escalation
First attempt: analyze source code and inject targeted instrumentation/logging. If the root cause remains unclear after 3 analysis iterations (going in circles), use GDB batch surveillance (`make debug-test`) to gather precise evidence — backtrace, register state, memory values. Do NOT switch back to source speculation once GDB is warranted.

## 14. Catching Stray Writes with lldb Hardware Watchpoints

### Problem
Test-isolation canary corruption (`canary_before`/`nu`/`canary_after` mismatch at
`snapshot_restore` entry) indicates a stray write to the snapshot buffer during the
previous test. GDB 17.2 has batch-mode hardware-watchpoint incompatibilities with
QEMU; use lldb instead.

---

## 14. Test Discipline for Kernel Development

### Targeted class verification (MANDATORY)
- Verify kernel code changes using the **smallest applicable test class**, NOT `all`.
- Examples: `vfs` for VFS/IPC/daemon changes, `buffer_pool` for buffer pool changes, `timer` for timer changes, etc.
- The `all` target runs 745+ tests and takes **minutes** — only use for full release validation.

### Full-suite log capture (MANDATORY)
- When running `make execute-test ... all`, always capture the full output:
  ```
  make execute-test x86_64 debug all 2>&1 | tee /tmp/jarvis-all-$(date +%Y%m%d-%H%M%S).log
  ```
- This preserves the complete test log for later inspection (failures, warnings, serial output).
- Without a log file, a truncated terminal buffer forces re-runs — wasteful.