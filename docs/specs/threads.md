# Shared-Address-Space Threads (issue #52)

TCB-per-thread scheduler integration: threads share one root PML4 and own
private stacks. Capability-gated via the `SHARE` right.

## 1. ABI

`SYS_THREAD_CREATE = 93` (FULL path only, never FAST):

| arg | meaning |
|---|---|
| arg0 | owner pid (`0` = self) |
| arg1 | entry PC (user-half, canonical, mapped + executable in the shared root) |
| arg2 | private stack bytes, page-rounded (1 .. `CONFIG_THREAD_MAX_STACK_SIZE`) |
| arg3 | Task-cap handle with `SHARE` (cross-task only; must be `0` for self) |

Self-spawn is capless (same authority as `FORK`). Cross-task spawn requires
a live `(Task, SHARE)` token in the caller's CSpace: the handle is the
authority token, the pid selects the validated target (`validate_id` +
`find_task` + magic). `SHARE` is never implied by `WRITE` (audit S2-1).

Fail-closed: bad pid/handle/entry/size, OOM, or admission denial all
return `-1` with zero side effects (validate-before-allocate, #400
discipline). Reachable failures return codes, never `ENSURE`.

`SYS_EXEC` refuses threads (`is_thread()` → fail): exec would swap the
shared root from under siblings. Threads exit; the group leader re-execs.

## 2. Lifetime

- One `ThreadAddrGroup` per shared root (static table,
  `CONFIG_THREAD_MAX_GROUPS = 16`): `{root, member_count, shared_pcid}`.
  Metadata lives independent of any TCB — owner death never frees a live
  root (audit S1(b)).
- All group mutations serialize under the group SpinLock taken under an
  `arch::IrqGuard` (leaf order IrqGuard → group lock; audit S2-1). The
  guard is held across the spawn map loop, the teardown own-stack unmap,
  and the atomic leave-to-free on a shared root — VMM map/unmap take no
  locks, so this guard IS the serialization against concurrent
  create/exit table alloc on one root. Last-member free runs inside the
  same section: the zero-count group can never be re-joined mid-free.
- Teardown unmaps + frees exactly the member's own stack pages, recycles
  its window slot, and leaves the group. The last member frees the root:
  `free_user_pages` walk (shared TEXT/DATA/HEAP + tables), the shared
  context ID (audit S2-4: share-while-live, free-on-last-exit), then the
  root page.
- Context switch is unchanged (audit S1(d) safe default): CR3 reloads
  every switch; sharing the PCID/ASID only tags the shared root correctly.

## 3. Address layout (audit S1(c))

```
0x60000000  HEAP (1 MiB)
0x61000000  MMIO window (8 x 2 MiB)
0x62000000  SHM window (8 x 2 MiB, ends 0x63000000)
0x64000000  THREAD window (64 slots x (64 KiB + guard page))
0x70000000  private STACK_VADDR (fork/ELF shape, unchanged)
```

Window bounds pinned by `static_assert` in `task.cpp` (disjoint from
HEAP/MMIO/SHM/STACK, no wrap). Thread stacks are user data (never
executable, #325-D) with a guard page below each slot.

## 4. Scheduling, signals, reaping (audit S2-3)

- Threads inherit priority/affinity, start `READY`, and are dispatched as
  ordinary TCBs (no scheduler-queue changes). Period/deadline are `0`
  (aperiodic, #312 precedent) — the Liu-Leyland bound is unchanged.
- Threads are linked as children of the spawner, so the proven
  zombie/`waitpid` reap path collects them: **`waitpid` MAY return thread
  tids**; `kill` addresses each thread per-TCB (fatal signals terminate
  exactly that thread — no group kill in v1).
- TLS: threads start with `tls_base_ = 0` and call `TLS_SET` first
  (#53 pthread glue contract).

## 5. v1 scope and follow-ups

- No target-bound `ThreadCap` (WdogCap-style `{pid, gen}` binding):
  cross-task spawn takes any live `SHARE` token + a validated pid. No
  production `SHARE` minters exist in v1, so cross-task spawn is
  fail-closed by construction. Binding granularity is a follow-up.
- Threads are user-only (`is_user_` required; kernel tasks rejected).
- `fork` from a thread deep-copies the whole shared root (correct,
  wasteful — same as any large address space).
- CR3-reload elision for same-root switches is a future optimization
  under a separate audit (v1 always reloads).
