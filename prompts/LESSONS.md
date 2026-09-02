# Lessons Learned

## 2026-06-21 — Nested Timer Interrupt Corrupts Scheduler State (GPF in cleanup_zombies)

### Root Cause
- The syscall handler's `sys_receive` uses `sti()` / `hlt()` / `cli()` to block waiting for IPC messages.
- A timer interrupt can fire during this window (after `sti`, before `cli`), nesting inside the outer interrupt context.
- The timer ISR calls `Scheduler::on_tick()` → `rate_monotonic_schedule()` → `switch_to_task()`, which sets up `scheduler_save_rsp_to` / `scheduler_load_rsp_from` and expects to perform the context switch on `iretq`.
- However, the outer syscall ISR was *also* mid-reschedule and had set up these same globals for its own context switch.
- The ISR assembly (`isr_stubs.asm`) was consuming the context-switch globals unconditionally, even for nested interrupts (`isr_nesting_depth > 1`). This caused the inner timer ISR to "steal" the context switch, overwriting `shell_task_ptr_` and other scheduler globals while the outer ISR was still using them.
- In `make test-qemu`, the system exits immediately after tests via `qemu_debug_exit(0)`, so the corrupted `shell_task_ptr_` is never dereferenced. In `make run-release`, the system continues running, daemon restart triggers `cleanup_zombies()`, which reads the corrupted pointer → GPF.

### Fix
- In `src/kernel/arch/x86_64/isr_stubs.asm`, added a check: `cmp qword [rel isr_nesting_depth], 1; jne .restore` before consuming `scheduler_save_rsp_to` and performing the context switch.
- Only the outermost interrupt (nesting depth == 1) performs the context switch. Nested interrupts skip the context-switch logic entirely.

### Lesson
- **ISR nesting must be tracked at the assembly level**, not just in C++ handlers. The `isr_nesting_depth` counter existed but the context-switch logic in the ISR epilogue didn't respect it.
- Any global state set up for deferred operations (like context switch on `iretq`) must be guarded against nested interrupt consumption.
- The `sti/hlt/cli` blocking pattern in syscalls is inherently racy with timer interrupts. The long-term fix is replacing it with lock-free IPC blocking (ROADMAP Phase 6).

## 2026-06-08 — O(1) Syscall Dispatch Refactoring

## 2026-06-08 — O(1) Syscall Dispatch Refactoring

### Table declarations must precede their use in constexpr inline members
- A `static constexpr` table initialized inline within a class body can only reference member declarations that appear *before* it in the class.
- All 37 handler method declarations must precede the `syscall_table_` initializer list, or compilation fails with "no member named" errors.
- The fix: move all `static uint64_t sys_*(...)` declarations above the `constexpr SyscallHandler syscall_table_[...]` member.

## 2026-06-07 — fork/exec Crash Debugging (SSE + Stack Alignment + Identity Map)

### CR4.OSFXSR must be set for SSE
- GCC at `-O2` emits SSE instructions (`movdqa`, `movaps`) for struct copies, string ops, etc.
- Without `CR4.OSFXSR` (bit 9), SSE instructions raise `#UD` (SIGILL, vector 6).
- `CR4.OSXMMEXCPT` (bit 10) should also be set for SIMD floating-point exception handling.
- Fix: `or dword [cr4], (1<<5)|(1<<9)|(1<<10)` in `boot.asm` (PAE|OSFXSR|OSXMMEXCPT).

### Always use higher-half mapping for physical→virtual conversion
- Kernel code must use `0xFFFF800000000000 + phys` for physical-to-virtual conversion, regardless of what PML4 is active.
- The identity-map at PML4[0] exists in the kernel PML4 (boot setup) but is **absent** in all user PML4s because `clone_kernel_pml4()` zeroes entries 0-255.
- Relying on the identity map is fragile — it is a boot artifact and should not be assumed for kernel code that runs across context switches.
- Affected components: PMM bitmap, VMM page table walks (`get_table`, `map_page_in_pml4`, `free_user_pages`, `clone_kernel_pml4`), ELF loader segment copies.

### free_user_pages must NOT be called on shared page tables
- Fork shares PDPT/PD/PT pages between parent and child (PML4 entries 0-255 point to the same physical page tables). Calling `free_user_pages()` on a shared PML4 would free the shared page table hierarchy, corrupting the other task's address space.
- Fix: `TaskControlBlock` has a `page_table_shared_` flag, set to `true` by `clone()`. Both `cleanup()` and `exec_into_current()` check this flag and skip `free_user_pages()` when set. The PML4 page itself is still freed.
- Consequence: per-fork allocations (user stack, PT entries for the new stack) under the shared hierarchy are leaked when the child exits. Acceptable because children always exec or exit quickly.

### waitpid return semantics
- If `waitpid` is called before the child terminates, the parent blocks. When the child exits, the scheduler must wake the parent.
- The current `WAITPID` handler returns `-1` if the child is still alive on first check, then the parent blocks and gets rescheduled. On re-entry after wakeup, the handler **may not re-check** the child state — can return `-1` incorrectly.
- Fix needed: always re-check child termination state on resume.

### HHDM circular dependency — page tables need HHDM to be zeroed, HHDM needs page tables
- When mapping a framebuffer at physical 0x80000000 (2 GiB) via HHDM (0xFFFF800080000000), the VMM must create page tables for the HHDM mapping.
- The boot HHDM only covers 0–128 MiB (PML4[256] → PDPT_HIGHER[0] with 64 × 2 MiB huge pages). Page table pages allocated above 128 MiB have no HHDM mapping yet.
- `VMM::get_table()` returned `HHDM_OFFSET + new_page` for the new page table page, but that virtual address was unmapped! Writing the PTE via that address caused a GPF.
- Temporary mapping via PML4[511] failed because the virtual address for PML4[511] mappings (0xFFFFFC0000000000+) was not set up — the code accessed via identity mapping instead.
- **Fix:** Reserve a 16 MiB pool in the first 128 MiB for page table pages (`PMM::alloc_page_table()`). These pages are guaranteed covered by the boot identity map (0–128 MiB) and boot HHDM huge pages. Zero them via the identity mapping (virtual = physical), return `HHDM_OFFSET + phys` which is valid because the page is in the boot HHDM range. Removed the broken PML4[511] temporary mapping entirely.
- **Lesson:** Page table allocation must guarantee that the page table pages themselves are mappable. Either allocate them from pre-mapped memory, or recursively create the HHDM mappings for them first.

### clone_kernel_pml4 must NOT copy identity-map; fork copies user entries from parent
- `clone_kernel_pml4()` is used by `elf::load()` and `exec_into_current()` to create a *fresh* user PML4. It zeroes entries 0-255 (user range) and copies 256-511 (kernel range). This ensures no boot identity-map pages leak into user PML4s.
- Fork (`TaskControlBlock::clone()`) needs the OPPOSITE: it allocates a raw PML4, copies user entries 0-255 from the PARENT's PML4 (sharing page tables), and kernel entries 256-511 from the kernel PML4.
- **First attempted fix** (broken): zero entries 0-255 in `clone_kernel_pml4()` and also call it from fork. This broke fork — the child's PML4 had no user mappings.
- **Second attempted fix** (broken): keep copying all 512 entries in `clone_kernel_pml4()` and make `free_user_pages()` tolerant of kernel-owned pages. This broke because the identity-map pages are supervisor-only, so user code within the identity-map range (e.g., 0x4011CF) gets a protection fault (#PF err=0x5).
- **Correct fix**: separate the two use cases. `clone_kernel_pml4()` yields a fresh PML4 (zeroed user range). Fork has its own PML4-construction logic that shares the parent's user entries.
- Key insight: a single "clone kernel PML4" function cannot serve both exec (fresh) and fork (shared) semantics.

## 2026-06-08 — VMM Page-Table Corruption: Three Interconnected Bugs + Uninitialized Garbage Entries

### Three bugs that masked each other
This crash cascade took over two hours to fully diagnose because **each bug masked the next**:

| Bug | What | Where | Masked by |
|-----|------|-------|-----------|
| #1 | PML4 accessed via identity mapping (`kernel_pml4_` as virtual) | `map_page`, `unmap_page`, `virt_to_phys` | All three |
| #2 | `get_table` zeroes new page tables via identity map (virtual=phys) — corrupts PMM metadata when PMM hands out a page above 128 MiB | `get_table` lines 21, 37 | Bug #1 (GPF in map_page kills boot first) |
| #3 | `get_table`/`map_page` don't handle 2 MiB huge pages at PD level — interprets pixel data as page-table entries | `get_table`, `map_page`, `map_page_in_pml4` | Bug #2 (page above 128 MiB hits identity-map fault first) |
| #4 | Uninitialized PDPT entries (indices 1–511) contain residual GRUB/BIOS data with PAGE_PRESENT bit set — walker follows garbage pointers to random physical memory | PDPT_IDENTITY[2], PDPT_HIGHER[2] at phys 0x2010, 0x4010 | Bug #3 (huge-page split saved us) |

**The chain of failure:**
1. Boot `higherhalf_entry` sets `rbp = old_rsp` (boot stack at ~0x7FEF8 — within 2 MiB identity map huge page at PD_IDENTITY[0]).
2. `Framebuffer::init()` → `map_page(HHDM_OFFSET + fb_page, …)` → PML4[256] → PDPT_HIGHER[2] has garbage with bit 0 set → `get_table` returns `HHDM_OFFSET + garbage_phys` as a PD pointer.
3. Writing to that garbage PD corrupts random kernel memory — in our case, it zeroes PD_IDENTITY[0], which maps the boot stack.
4. Later in `higherhalf_entry`, `[rbp-0x30]` writes to 0x7FEC8 → page fault (err=0x2) because the huge page was split/corrupted.

### Why the huge-page split fix (#3) made things worse before #4 was found
Fixing bug #3 (splitting huge pages in `get_table`) was necessary for the HHDM framebuffer mapping to work. But it exposed bug #4: once the GPF stopped (from bugs #1–#3), the code now reached the ELF-loading stage, which accessed the boot stack via RBP, which hit the now-corrupted PD_IDENTITY[0].

Without the bug #4 fix (PDPT zeroing), the huge-page split code writes to the garbage PD. With bug #4 fixed, PDPT entries are zeroed → `get_table` correctly allocates new page tables → no corruption.

### Fixes applied
| Fix | File | What changed |
|-----|------|-------------|
| HHDM everywhere | `vmm.cpp` | All PML4 accesses use `HHDM_OFFSET + (kernel_pml4_ & ~0xFFF)` instead of raw physical address |
| HHDM for zeroing | `vmm.cpp:37` | `new_table = HHDM_OFFSET + new_page` instead of raw phys |
| Huge page splitting | `vmm.cpp:17-28` (get_table), `vmm.cpp:73-84` (map_page), `vmm.cpp:155-166` (map_page_in_pml4) | Split 2 MiB page into 512 × 4 KiB entries |
| PDPT entry zeroing | `vmm.cpp:14-33` (init) | Zero PDPT_IDENTITY[1-511] and PDPT_HIGHER[1-511] once at boot |

### Why the boot.asm zeroing approach was rejected
The first attempt zeroed the entire page-table page range (0x1000–0x5FFF) in 32-bit mode in `boot.asm`. This **hung the boot** because GRUB's multiboot info structure is often placed within that physical range. Zeroing it destroyed the multiboot info before the kernel could parse it. Fixing in `VMM::init()` (running in 64-bit mode with HHDM active) avoided this because by then the multiboot info had already been read.

### How to catch this kind of bug earlier
1. **Sanity-check page table entries**: Assert that PDPT/PD entries point to allocated pages (not garbage). A `get_table` debug mode could verify `entry & ~0xFFF` is a known PMM page.
2. **Poison uninitialized page table memory**: The boot code should zero its page table pages or the kernel should explicitly verify all entries before use.
3. **Mark identity-map pages as non-executable**: The boot stack is in the identity-map range. If identity-map pages were NX, corruption would cause a different crash signature, but it would still crash.
4. **Add RBP-stack validation**: Before using RBP-relative accesses after a stack switch in `higherhalf_entry`, verify the stack address is canonical and mapped. Or set `rbp = rsp` after the switch.
5. **Teach the QEMU test runner to capture panic output**: Currently `test-qemu` only checks for "Failed: 0" / "FAILED". On panic the kernel prints registers and a fatal message — capture and display this on failure.

## 2026-06-08 — Shell Crash Cascade: Task Resurrection + remove_task Corruption

### The symptom
After boot, the shell would rapidly print garbage, followed by cascading GPF errors across all kernel tasks. The QEMU test runner got stuck — serial output stopped before `make test-qemu` could complete.

### Root Cause: Two independent bugs that amplified each other

| Bug | What | Where | Effect |
|-----|------|-------|--------|
| #1 | `remove_task` swap-and-pop does not update `current_index_` | `scheduler.cpp:46` | `current_task()` returns out-of-bounds memory; `sys_exit` skips cleanup → task spins in `pause;jmp` infinite loop |
| #2 | All sync primitives set `state = READY` without checking TERMINATED | `ipc.cpp:177`, `notify.cpp:16`, `queue.cpp:34,44`, `eventgroup.cpp:36`, `mutex.cpp:28`, `semaphore.cpp:27,30` | A killed task gets resurrected when a sender/notifier wakes it → zombie task executing freed memory → GPF |

**Bug #1** alone: task `_exit()` calls `remove_task`, which finds the task and swaps it with the last element, then `pop()`. But `current_index_` still points to the old index. If `current_index_` > `pop`ped index, it's now off-by-one. `current_task()` returns `&tasks_[current_index_]` which may point past the vector or to a different task's TCB. `sys_exit` then checks `current_task()->state != TERMINATED` → sees a different task's state → skips cleanup → returns from `int $0x80` → task loops forever in `pause;jmp` → hogs CPU but doesn't crash.

**Bug #2** alone: a task exits → remove_task → freed memory. Meanwhile, another task sends IPC to the (now killed) pid. `wake_sender` iterates the queue, finds the killed task's TCB, and sets `state = READY`. The scheduler now sees a task that should not exist and schedules it → it executes freed stack → GPF.

**Together**: Bug #1 keeps the exiting task from being cleaned up (it stays in the task list with TERMINATED state). Bug #2 resurrects it. The resurrected task runs on freed/corrupted stack → GPF. The GPF handler calls `task_kill`, which calls `sys_exit` again → `remove_task` corrupts `current_index_` again → more bugs. Cascade.

### Fixes applied (5 files)

| Fix | File | What changed |
|-----|------|-------------|
| remove_task current_index_ | `scheduler.cpp:46-54` | After swap-and-pop, decrement `current_index_` if it points past the end, or if the removed task was before current |
| TERMINATED guard (sync) | `ipc.cpp:177`, `notify.cpp:16`, `queue.cpp:34,44`, `eventgroup.cpp:36`, `mutex.cpp:28`, `semaphore.cpp:27,30` | All wake/swake operations check `task->state != TERMINATED` before setting `state = READY` |
| TERMINATED guard (parent wake) | `syscall_handlers_misc.cpp:86` | `sys_exit` parent-wake checks `state != TERMINATED` before `unpark` |

### Incorrect IDT analysis
Initial analysis blamed a "trap gate" vs "interrupt gate" issue — that `0xEE` bit 4 being set would prevent IF from being cleared during syscall handling. This was wrong:

- `0xEE = 1110 1110`: P=1, DPL=11 (ring 3), bit 4=0 (reserved, must be 0), type=1110 (0xE = 32-bit interrupt gate). This **is** an interrupt gate that clears IF on entry.
- The intended "fix" `0xCE = 1100 1110` has DPL=10 (ring 2), which causes GPF on `int $0x80` from userspace. Reverted.
- The original `0xEE` was correct for a DPL-3 interrupt gate. The crash cascade was caused by bugs #1 and #2 alone.

### Key lesson: 0xEE vs 0xCE vs 0xEF

| Value | Bit 4 | Type (bits 3‑0) | DPL (bits 6‑5) | Gate type | DPL |
|-------|-------|-----------------|----------------|-----------|-----|
| `0xEE` | 0 | 0xE (interrupt) | 11 (ring 3) | 32-bit interrupt gate | 3 ✓ |
| `0xCE` | 0 | 0xE (interrupt) | 10 (ring 2) | 32-bit interrupt gate | 2 ✗ |
| `0xEF` | 0 | 0xF (trap) | 11 (ring 3) | 32-bit trap gate | 3 ✓ |

The "reserved bit" (bit 4) must be 0. The interrupt-vs-trap distinction is bit 3 of the type field.

### Bonus fix: userspace shell improvements

| Fix | File | What |
|-----|------|------|
| `parse_line` use-after-return | `sh.c:44` | `argv` pointers referenced `buf[]` on `parse_line`'s stack (destroyed on return). Changed to work on global `line[]`. | 
| `read_line` empty-input exit | `sh.c:185` | `<= 0` → `< 0` so a stray newline from stale keyboard buffer doesn't exit the shell |
| Keyboard flush at boot | `devfs.cpp:15-19` | Calls `arch::Keyboard::flush()` to discard boot-time scancodes before shell reads /dev/tty |
| /dev/tty polling | `devfs.cpp:20-40` | `tty_read` polls both COM1 serial and keyboard ring buffer for input |
| `_exit`/`abort` hlt | `unistd.c:54`, `stdlib.c:9` | `hlt` → `pause` (`hlt` is privileged in ring 3 → GPF) |

## 2026-06-21 — Scheduler Starvation: Daemons Ping-Pong, Shell Starves

### Root Cause
- `Scheduler::next_task()` used `task != current` as a tiebreaker for equal-priority/period tasks, causing vfsd and iocd (both priority=1, period=10) to continuously preempt each other.
- The shell (priority=1, period=20) had the same priority but longer period, so it never got CPU time.
- `next_task()` had no round-robin mechanism for equal-priority/period tasks.

### Fixes Applied

| Fix | File | What changed |
|-----|------|-------------|
| Round-robin tiebreaker | `scheduler.cpp:next_task()` | Replaced `task != current` with `remaining_ticks` comparison: when priority/period are equal, pick the task with fewer remaining ticks (been waiting longer). If both have same remaining ticks and current is `best`, pick the other task for round-robin. |
| Shell priority/period | `kernel.cpp:517` | Changed shell from `priority=1, period=20` → `priority=2, period=5` (rate-monotonic correct: shortest period, highest priority) |
| Liu-Leyland LUB admission | `scheduler.cpp:add_task()` | Added static LUB table and `is_rm_schedulable()` check that warns when total periodic utilization exceeds `n * (2^(1/n) - 1)` |

### New Task Parameters

| Task | Priority | Period (ticks) | Note |
|------|----------|----------------|------|
| Idle | 0 | ∞ (infinite) | Never preempts |
| vfsd | 1 | 10 | Same bucket as iocd |
| iocd | 1 | 10 | Round-robin with vfsd |
| Shell | 2 | 5 | Highest priority, shortest period |
| test_fork | 1 | 50 | Low-rate task |

### Lesson
- Rate-monotonic scheduling requires **strict prioritization** by period: shorter period = higher priority. When two tasks share the same priority and period, a round-robin tiebreaker is needed.
- The `task != current` hack in `next_task()` caused pathological ping-pong. The correct approach: use `remaining_ticks` (higher = just started, lower = been running) for round-robin.
- The Liu-Leyland LUB bound `n * (2^(1/n) - 1)` provides a sufficient schedulability test. Current boot tasks exceed the bound (100% CPU-bound assumption), but block on IPC in practice.
