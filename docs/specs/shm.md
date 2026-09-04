# Capability Shared-Memory Granules — Zero-Copy Ring (issue #106 Part B)

**Doc ID:** NEX-SPEC-2026-09-03-106
**Status:** IMPLEMENTED (issue #106 Part B, v0.4.3 — 2026-09-03)
**Milestone target:** v0.4.3
**Related:** `docs/specs/ipc.md` (priority-ordered wake, Part A),
`src/kernel/cap/frame_map.{hpp,cpp}`, `src/kernel/ipc/shm_ring.hpp`,
`src/kernel/cap/frame.{hpp,cpp}`, `docs/specs/cspace.md`.

## Purpose

A capability-gated, genuinely zero-copy shared-memory ring for high-throughput
client-server IPC.  A producer and a consumer each map the SAME physical
frames (backed by a `FrameCap`) into their user VA window via
`SYS_FRAME_MAP`; after mapping, data flows directly between the two user
spaces through the shared frames with **no kernel involvement** (zero syscalls
on the data path).  The capability is the single authority gate: ownership,
grant, and revocation are enforced by the CSpace machinery (issues #1–#10).

## Design

### FrameUserMap registry (`cap/frame_map.{hpp,cpp}`)

Mirror of `MmioUserMap` (issue #8):

- Static bounded slot table (`CONFIG_CAP_MAX_FRAME_MAPS`, default 8), each slot
  keyed by `owner_task_id` + `owner_gen` (recycled-slot stale-VA defense).
- Each slot maps into a fixed user VA window:
  `CONFIG_USER_SHM_VA_BASE + slot * CONFIG_USER_SHM_REGION_SIZE`
  (base `0x62000000`, region `2 MiB`).  The window is statically pinned:
  above the heap (`0x60100000`), above the MMIO window (`0x61100000`), below
  the user stack (`0x70000000`).
- The registry holds a reference (`acquire()`) on the backing `FrameCap` for
  the lifetime of a live slot — the VMM unmap path can never dereference a
  freed cap (UAF, mmio.cpp precedent).
- Lock discipline: claim/clear/revalidate under `s_lock_`; VMM map/unmap
  (which may allocate PMM page-table pages) OUTSIDE `s_lock_`.  Only the path
  that observes the slot's `occupied true->false` transition releases the
  slot pin (double-release → premature dispose → UAF guard, identical to
  mmio.cpp:186-198/236-246).
- Revocation closure: `FrameCap::dispose()` / `revoke()` call
  `FrameUserMap::invalidate_cap(this)` to retroactively unmap every live
  mapping backed by the cap before the cap block is freed.
- Task-death drain: `TaskControlBlock::cleanup()` and `exec_into_current`
  call `FrameUserMap::drain_task` (beside `MmioUserMap::drain_task`) so a
  recycled PML4 never carries stale shared-memory PTEs.

### Syscalls (`syscall_handlers_shm.cpp`)

| Syscall | Number | Signature | Semantics |
|---|---|---|---|
| `SYS_FRAME_CREATE` | 63 | `(count)` → slot index | Allocates `count` contiguous USER-owned frames (`PMM::alloc_user_contiguous`), wraps them in a `FrameCap`, installs it in the caller's root CNode (R/W), returns the slot. Rollback on failure. |
| `SYS_FRAME_MAP` | 64 | `(cap_handle)` → VA | `cap::lookup` a `Frame` cap with `CAP_RIGHT_WRITE`; `FrameUserMap::map` into the caller's VA window. |
| `SYS_FRAME_UNMAP` | 65 | `(va)` → 0/-1 | `FrameUserMap::unmap` (owner+generation revalidated). |

These syscalls touch page tables and MUST stay out of `k_syscall_fast[]`
(issue #92 discipline — a canary-skip on a page-table-touching path would be
a privilege hole).  `MAX_SYSCALL` 63 → 66.

### Shared ring protocol (`ipc/shm_ring.hpp`)

- `SharedRingHeader` lives at the **start of page 0** of the `FrameCap`:
  magic (`0x4E455852494E47`), version, `capacity`, `element_size`, plus
  cache-line-aligned (`alignas(64)`) `head` (producer-write-only) and `tail`
  (consumer-write-only).
- Data lives in pages 1..N-1.  Ring capacity = `(frame_count - 1) * PAGE_SIZE`
  rounded down to a power of two (so the wrap mask works).
- `shm_ring_init` is called once by the producer before either side uses the
  ring.  `shm_ring_push` / `shm_ring_pop` follow the SPSCRing acquire/release
  protocol (head published with release after the payload write; tail
  published with release after the payload read).  Single-producer /
  single-consumer.
- Element size is a fixed multiple of 8.  Head/tail are in bytes; the element
  index is `head / element_size`.

### Revocation semantics

- `SYS_CAP_REVOKE` → `FrameCap::revoke()` → `FrameUserMap::invalidate_cap`
  unmaps EVERY live mapping backed by the cap (all owners), then marks the cap
  revoked (acquire() refuses).  Frames are freed only on the last reference
  (dispose).
- `SYS_FRAME_UNMAP` removes only the calling task's mapping.
- Task death/exec: cleanup drains the task's mappings.

## Invariants

- INV-SHM1: no dynamic allocation on the map/unmap/push/pop paths (static
  bounded registry + fixed VA window + fixed header).
- INV-SHM2: the capability is the only authority — `cap::lookup` (type +
  rights + generation) gates every `SYS_FRAME_*`; a revoked cap refuses map.
- INV-SHM3: slot pin released exactly once (occupied true→false transition
  observer only).
- INV-SHM4: `drain_task` runs before `free_user_pages` at both call sites
  (never walk a freed PML4).
- INV-SHM5: ResourceTracker zero-delta — `FrameCap` create/dispose balance
  cap_objects; slot install/remove balance cap_slots; frame alloc/free
  balance pmm_pages_used.

## Test Plan (landed — class `cap_shm`, 5 TF_KERNEL)

1. `frame_user_map_unmap_roundtrip` — real-PML4 task maps a FrameCap via
   SYS_FRAME_MAP, unmaps via SYS_FRAME_UNMAP; live_count returns to 0.
2. `frame_user_map_revoke_denied` — revoked FrameCap refuses SYS_FRAME_MAP.
3. `shm_ring_producer_consumer` — producer maps + writes a 32-element ring
   into its cloned PML4; the harness proves zero-copy (mapped VA →
   `virt_to_phys_in_pml4` == the ring phys) and reads the ring back via the
   physical frames (HHDM alias — the same frames a consumer would read),
   verifying payload + order.
4. `shm_ring_task_death_drain` — a mapping dies with its task; the registry
   drains (live_count baseline).
5. `shm_ring_revocation_cleanup` — revoking the FrameCap clears the live
   mapping + drops its pin (live_count baseline).

## Non-Goals

- Read-only (RO-PTE) mappings for the consumer — both sides map RW today;
  capability rights still gate *who* may map.  RO-PTE is a documented follow-on.
- A user-space `SYS_FRAME_CREATE` grant flow across CNodes (grant/copy already
  exist — the test installs the cap in the producer's CSpace directly).

## Pairing with the Crash Supervisor (issue #105 Part B)

The async task-death notification mechanism (`docs/specs/death-notify.md`,
SYS_DEATH_WATCH/RECV/UNWATCH) is the supervisor wakeup that pairs with this
ring for client-server fault recovery: when a ring producer/consumer dies, the
supervisor's death pulse fires, it drains the death record, and the ring frames
are reclaimed via `FrameUserMap::drain_task` (already in the dying task's
`cleanup()`) before `free_user_pages` — no stale PTEs, no leaked frames.
- SMP multi-producer/multi-consumer rings — the protocol is SPSC; the cache-line
  alignment of head/tail prepares for a future SMP migration.