# Syscall Fast Path — Asm Jump Table Dispatch

**Doc ID:** NEX-SPEC-2026-08-23-001
**Status:** DRAFT
**Milestone target:** v0.4.3
**Inspiration:** Cyjon `kernel/service.asm` (static `dq kernel_service_*`
table indexed by syscall number directly in assembly).
**Related:** `docs/specs/boundary.md`, `docs/specs/cspace.md`,
`src/kernel/syscall/syscall_entry.asm`, `src/kernel/syscall/syscall.cpp`

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

### 3.1 Tiered dispatch (new)

Split syscalls into two classes at build time:

| Class | Members | Entry validation |
|---|---|---|
| FAST | YIELD, SEND, RECEIVE, SEND_SYNC, NANOSLEEP, HALT | bounds check only |
| FULL | everything touching pointers/fds (OPEN, READ, …) | canary + AC check |

Implementation:

- `CONFIG_INCLUDE_SYS_*` tuners already produce `CONFIG_SYSCALL_COUNT`.
  Add a generated constexpr bitmask `SYSCALL_FAST_MASK` (one bit per number,
  derived from the same tuner list in configuration.md §1.6).
- In `syscall_entry.asm`: after the GPR frame is pushed, load the mask
  address (passed as a linker symbol) and test bit `rax`; if set, jump to a
  lean path that calls `syscall_handler_fast(number, args…, regs)` — same
  table, no canary walk. If clear, take today's full path unchanged.
- Canary verification moves from "every syscall" to "on FULL-path entry +
  context switch + timer tick sample", which restores its original purpose
  (detecting corruption) without taxing every yield.

### 3.2 Why not full-asm dispatch like Cyjon

A pure-asm `call [table+rax*8]` saves one C frame (~20 cycles) but forces us
to reimplement bounds-check, AC-check and signal-pending checks in assembly —
three audited code paths duplicated, exactly the kind of drift the boundary
spec forbids. The tiered approach keeps ONE dispatcher implementation in C and
only adds a bitmask test in asm.

## 4. Semantics / Invariants

- INV: every syscall still passes exactly one bounds check before table
  indexing (out-of-range ⇒ `-1`, unchanged).
- INV: FAST-class syscalls never dereference user pointers (verified list
  review required when a syscall changes class; add a static_assert-style
  test that FAST members are pointer-free handlers).
- Worst-case entry latency becomes bimodal and *documented*: FAST ≤ ~150
  cycles post-change, FULL unchanged. Deadline analysis uses the FULL figure
  as the upper bound.

## 5. Test Plan

New class `syscall_fastpath`:
1. `fast_mask_matches_config` — for every enabled INCLUDE_SYS tuner, mask bit
   matches the FAST/FULL classification table.
2. `fast_call_correctness` — drive YIELD/NANOSLEEP through both paths
   (mask forced off via test hook) and compare return values.
3. `full_path_still_validates` — plant a corrupted canary; a FULL syscall
   trips (latched in test mode), a FAST syscall does not consult canaries.
4. Latency histogram: measure entry-to-handler cycles for both classes,
   assert FAST < FULL in release build.

Validation: `selftest`, debug/release `all`, test-history rows per AGENTS.md.

## 6. Non-Goals

- `sysret`-level tuning (IA32_FMASK, speculative barriers) — separate effort.
- Changing the syscall ABI numbering (cspace.md owns SYS_CAP_* extensions).
