# NexIOS Audit Report — IPC / Capabilities / Syscalls

**Scope:** `src/kernel/ipc/*`, `src/kernel/cap/*`, `src/kernel/syscall/*` (~5,100 LOC)
**Date:** 2026-08-22 · **Baseline:** v0.4.2-dev (commit `624b9e5f`) · **Method:** static review
**Focus:** refcount/cap lifecycle correctness, IPC deadlock potential (priority inversion, blocked-sender queues), syscall input validation, error-handling consistency, API design

---

## Findings — HIGH

### H-1. `syscall_entry.asm:43–48` — argument shuffling destroys arg2 and `regs`
`mov rdx, rcx` overwrites rdx *before* `mov rcx, rdx` executes → both registers end up holding the old rcx; the third user parameter (rdx) is lost. Worse, `r9` (the fifth C parameter, `regs`) is loaded with the syscall *number* (`mov r9, rdi` after rdi=rax). Consequence: in `Syscall::handle` (syscall.cpp:119) the canary path dereferences `regs[17]` → dereferencing address "syscall-number+136" → fault/panic. The libc (`src/libc/syscall.h:78`) uses `int $0x80` (the correct path via kernel.cpp:1477), but any user code entering via the LSTAR/sysret entry gets wrong arguments. Latent, exploitable correctness/crash bug.

### H-2. BufferPool `map()` breaks ownership — buffer_pool.cpp:367–384
Unlike `free`/`unmap`/`transfer`, `map()` does **not** check `owner_task == task.id` and instead sets `owner_task = task.id` (line 377). Any task that knows a handle value can take over the buffer. Worse: if the buffer is still mapped in the original owner's address space, only `mapped_va` is overwritten — a later `free()` deletes the PTE in the wrong address space, leaving the old user mapping alive and pointing at recycled pages (cross-task UAF / information leak).

### H-3. Endpoint lifecycle / UAF: endpoint.cpp:46–50 vs. ipc.cpp:40–54
`MessageQueue::~MessageQueue` wakes blocked senders; `Endpoint::dispose()` does not (the embedded queue destructor never runs because only `MemPool::free` is called). A sender blocked in `IPC::send_via_cap` (ipc.cpp:289) on a full endpoint queue holds a raw `ep` pointer; on last release → `dispose()` → memory freed, and the sender writes into freed memory upon wakeup (`ep->q.is_full()`, ipc.cpp:299). The comment at endpoint.hpp:38–39 ("blocked senders must be drained before last release") is not enforced by code.

### H-4. No TLB flush after PTE teardown
`clear_pte_in_pml4` (buffer_pool.cpp:42) clears PTEs without `invlpg`/`tlb_flush` (VMM has `tlb_flush`; this path bypasses it). Stale TLB entries keep granting access to freed/recycled pages until random eviction — a genuine security hole for an RTOS with isolation claims.

## Findings — MEDIUM

### M-1. `send_sync` reply ambiguity + no timeout (ipc.cpp:336–390)
The "reply" is simply the next message on the caller's own queue (highest priority!) — any third-party or different-session message gets consumed as the reply. No timeout → a malicious/broken server blocks the client forever; no abort on signal/task termination of the client itself.

### M-2. Self-send deadlock guarded in `send()` but not in `send_via_cap`
ipc.cpp:181 has the self-send guard; ipc.cpp:277–301 lacks it → a task blocks forever on its own full endpoint.

### M-3. Silent buffer drop on failed transfer (ipc.cpp:217–219)
If `BufferPool::transfer` fails, `buf_handle=0` is set and the message is sent anyway — the receiver cannot detect the data loss; the sender receives `true`.

### M-4. Priority-inversion residue
`block_sender` boosts the owner (ipc.cpp:488–504), but `MessageQueue::~MessageQueue` wakes senders without undoing the boost → the owner keeps elevated priority permanently. Additionally, busy-wait blocking (ipc.cpp:192–195: BLOCKED task spins with `pause()`) burns budget and can produce deadlock-like delays under RMS; the interrupts-off path rolls back correctly, but there is a window between `block_sender` and the check.

### M-5. `sys_receive` violates the documented WEDGE invariant
syscall_handlers_ipc.cpp:83–85 sets `state = BLOCKED` **without** `dequeue_ready()` — exactly the pattern that the block in ipc.cpp:402–462 declares as a [WEDGE]-violation. Inconsistency between the two blocking paths.

### M-6. VA validation incomplete in BufferPool
`alloc`/`map` (buffer_pool.cpp:315,372) only check `va < USER_SPACE_LIMIT` — no page-alignment check, no collision check (mapping the same VA twice silently overwrites PTEs; a later `free()` then deletes another buffer's PTE).

### M-7. Error-handling patterns bypassed
`ipc_errors.hpp`, `buffer_pool_errors.hpp`, `syscall_errors.hpp` define clean X-macro codes, but **all** handlers return `(uint64_t)-1` (e.g. syscall_handlers_ipc.cpp:48,52; cap/sync/fs likewise). `SYS_ERR_*` is used nowhere outside tests. Dead code + users cannot distinguish failure causes.

### M-8. `cap::grant` mint-once race (cap.cpp:307–309)
`clear_grant(handle_slot(src_handle))` re-checks no generation — if the slot was removed/re-populated between rights snapshot and clear, GRANT is cleared on a foreign, new capability (or stays set on the actual source).

### M-9. Handle cspace field too narrow (cap_types.hpp:50/91)
Only 8 bits for cspace_id; CSpaces with id ≥ 256 can never resolve their own handles (`lookup` always fails, cap.cpp:170) → availability bug; no forgery risk since checked against own id.

## Findings — LOW / Design Notes

- `entries[].refcount` (buffer_pool.hpp) declared and initialized but never incremented — dead refcount architecture; lifecycles rely purely on single-owner + generation. For v0.4.x capability expansion, decide: real refcounts or remove the field.
- `MessageQueue::pop` is O(n) scan + O(n) bitmap rebuild per pop (ipc.cpp:84–138) → O(n²) under queue flooding by a malicious task.
- `capture_state`/`restore_state` ignore `max_bytes` entirely (buffer_pool.cpp:429–459) — test-only, but an overflow waiting to happen.

## Positive Observations

- Consistent handle-generation validation (`BufferPool::validate`, `cap::lookup` with gen+rights+acquire under spinlock).
- Thorough `checked()` pointer validation with SMAP-safe copy in the IPC syscalls.
- AC-leak detector and canary check at dispatch entry (syscall.cpp:99–132) are solid.
- `retype()` (untyped.cpp) has correct claim/rollback and fails closed.
- Capability releases consistently run outside spinlocks; lock ordering documented.

## Assessment

The **capability layer is the most robust part of the kernel** (pinning, generation checks, rights capping). IPC suffers from reply ambiguity and inconsistent blocking paths; the syscall layer validates pointers well but throws away its own defined error-code system. The sysret-entry argument bug (H-1) and the BufferPool ownership break combined with the missing TLB flush (H-2/H-4) are the top priorities to fix.
