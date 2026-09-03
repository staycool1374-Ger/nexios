# Syscall Fast Path — Tiered Dispatch

**Doc ID:** NEX-SPEC-2026-08-23-001
**Status:** IMPLEMENTED (issue #92, v0.4.3 — 2026-09-03)
**Milestone target:** v0.4.3
**Inspiration:** Cyjon `kernel/service.asm` (static `dq kernel_service_*`
table indexed by syscall number directly in assembly).
**Related:** `docs/specs/boundary.md`, `docs/specs/cspace.md`,
`src/kernel/syscall/syscall.cpp`, `src/kernel/syscall/syscall.hpp`

## 0. Implementation reconciliation (issue #92)

The tiered dispatch landed in C (not the dead `syscall_entry.asm`) because the
live syscall entry is `int $0x80` → trap-gate ISR → `syscall_handler` (kernel.cpp)
→ `Syscall::handle`. Key deltas from the DRAFT:

- **Bit-test location:** the FAST-bit test lives at the top of `Syscall::handle`
  (C), not in `syscall_entry.asm` (unassembled dead file). A future asm entry
  would reuse `Syscall::handle_fast` via a thin `extern "C"` wrapper.
- **FAST membership (reconciled to the pointer-free set):** the issue's literal
  list {YIELD, SEND, RECEIVE, SEND_SYNC, NANOSLEEP, HALT} is NOT implementable:
  SEND/RECEIVE/SEND_SYNC deref user buffers via `checked_ptr` /
  `safe_copy_to_user` (syscall_handlers_ipc.cpp), and NANOSLEEP does not exist
  in the enum. The landed FAST set is the audited pointer-free subset:
  **{YIELD, GET_TICKS, PRINT, CREATE_MAILBOX, DESTROY_MAILBOX, GETPID, PAUSE,
  REBOOT, HALT}** (`Syscall::k_syscall_fast[]`, the single source of truth;
  `SYSCALL_FAST_MASK` derived from it). IPC rejoins FAST only behind a
  follow-up that first lands canary-on-context-switch (done here) + a pointer
  deref review.
- **Canary relocation (INCLUDED):** canary verify now runs on FULL-path entry
  AND in the scheduler (`canary_check_in_scheduler_hooks`) at every context
  switch to a user task plus a bounded timer-tick sample
  (`CONFIG_CANARY_SAMPLE_TICKS`, default 64). This restores corruption
  detection for FAST-only tasks without taxing the hot syscall path.
- **AC check:** the debug-only MP-4 AC-leak check is RETAINED on both FAST and
  FULL (the DRAFT said "bounds check only") — a ~2-cycle debug read that keeps
  the MP-4 leak detector; deviation documented here.
- **`CONFIG_INCLUDE_SYS_*` / `CONFIG_SYSCALL_COUNT` are dead code:** the mask
  derives from the `SyscallNumber` enum list, not the tuners.

## 1. Current State (verified)

`syscall_entry.asm` pushes the full GPR frame, shuffles registers into the
SysV argument order and calls the C function `syscall_handler`, which forwards
to `Syscall::handle`. The dispatch itself is already table-based:

```cpp
// syscall.cpp:229 — constexpr table, MAX_SYSCALL bound-checked first
return syscall_table_[number](arg0, arg1, arg2, arg3, regs);
```

So the *lookup* cost is already O(1). What remains on the hot path before any
handler runs:

1. `number >= MAX_SYSCALL` bounds check (C),
2. debug-only SMAP AC-flag check (`read_rflags`, debug builds only),
3. `CONFIG_CANARY_GUARD` canary verification of all user segments —
   **page-table walks on every single syscall**, production too,
4. C call overhead + register shuffle done twice (asm→SysV args, then the
   compiler's own save/restore).

## 2. Problem

For hard real-time claims the worst-case syscall entry cost matters, not the
average. Today the canary walk (step 3) sits in front of *every* dispatch,
including the hottest calls (`YIELD`, IPC send/receive, NANOSLEEP). Cyjon's
lesson is the opposite extreme — zero validation, pure `call [table+rax*8]` —
which we must NOT copy (it violates our boundary principle "validate at the
boundary, once" and would break MP-3/MP-4 guarantees).

## 3. Design

Keep validation in C (it is correct and audited); make the *path* cheaper:

### 3.1 Tiered dispatch (implemented)

Split syscalls into two classes:

| Class | Members (landed) | Entry validation |
|---|---|---|
| FAST | YIELD, GET_TICKS, PRINT, CREATE_MAILBOX, DESTROY_MAILBOX, GETPID, PAUSE, REBOOT, HALT | bounds check + debug AC check |
| FULL | everything touching pointers/fds (OPEN, READ, SEND, RECEIVE, SEND_SYNC, …) | canary + AC check |

Implementation (as landed):

- `constexpr SyscallNumber k_syscall_fast[]` is the single source of truth;
  `constexpr uint64_t syscall_fast_mask_from()` folds it into
  `SYSCALL_FAST_MASK` (static_assert: non-empty, no bit >= MAX_SYSCALL).
- `Syscall::handle` bounds-checks first, then tests the FAST bit and
  dispatches inline (same `syscall_table_`) without the canary walk; the FULL
  path keeps AC + canary. `Syscall::handle_fast` is the self-contained lean
  path (bounds + dispatch) reused by tests and a future asm entry.
- Canary verification moved from "every syscall" to "FULL-path entry +
  context switch + timer tick sample" (`canary_check_in_scheduler_hooks`,
  `CONFIG_CANARY_SAMPLE_TICKS`), restoring its corruption-detection purpose
  without taxing every yield.
- The debug-only AC check stays on BOTH paths (MP-4 leak detector preserved;
  ~2 cycles, debug builds only).

### 3.2 Why not full-asm dispatch like Cyjon

A pure-asm `call [table+rax*8]` saves one C frame (~20 cycles) but forces us
to reimplement bounds-check, AC-check and signal-pending checks in assembly —
three audited code paths duplicated, exactly the kind of drift the boundary
spec forbids. The tiered approach keeps ONE dispatcher implementation in C and
only adds a bitmask test in asm.

## 4. Semantics / Invariants

- INV: every syscall still passes exactly one bounds check before table
  indexing (out-of-range ⇒ `-1`, unchanged).
- INV: FAST-class syscalls never dereference user pointers (enforced by the
  audited `k_syscall_fast[]` membership; SEND/RECEIVE/SEND_SYNC excluded
  because they touch user buffers; a syscall may only join FAST after a
  pointer-free handler review + list edit + `fast_mask_matches_config` gate).
- INV: canary detection is NOT weakened by the relocation — FAST-only tasks
  are still sampled at every context switch and on a bounded tick cadence.
- Worst-case entry latency becomes bimodal and *documented*: FAST = bounds +
  AC (no canary page-table walk), FULL unchanged. Deadline analysis uses the
  FULL figure as the upper bound. Measured (debug TCG, kernel-context, N=2000):
  FAST sum 556K vs FULL sum 752K cycles (FAST faster; no absolute bound
  asserted — TCG rdtsc quantization makes the relative comparison the honest
  signal; the dominant canary-walk delta is proven functionally by
  `full_path_still_validates` / `fast_path_skips_canary`).

## 5. Test Plan (landed — class `syscall_fastpath`, 5 TF_KERNEL tests)

1. `fast_mask_matches_config` — mask == the pointer-free list (popcount), all
   members < MAX_SYSCALL, non-empty (compile-time static_asserts + runtime).
2. `fast_call_correctness` — call-safe FAST members through both paths (hook
   off) return identically; out-of-range returns -1 on both.  PAUSE/HALT/
   REBOOT excluded (they halt the CPU — never return).
3. `full_path_still_validates` — tampered stack canary + FULL syscall trips
   the latch (MP-3 preserved; with the relocation the trip may latch at the
   context-switch/tick sample instead — either way it fires).
4. `fast_path_skips_canary` — a FAST-only user task (yield-probe) with a
   tampered canary runs without faulting at syscall entry; detection is via
   the scheduler hooks, never a user-pointer dereference.
5. `fast_latency_lt_full` — relative latency (sum_fast <= sum_full) with a
   magnitude-sanity canary; no absolute cycle bound.

Validation: `selftest`, debug/release `all`, test-history rows per AGENTS.md.

## 6. Non-Goals

- `sysret`-level tuning (IA32_FMASK, speculative barriers) — separate effort.
- Changing the syscall ABI numbering (cspace.md owns SYS_CAP_* extensions).
