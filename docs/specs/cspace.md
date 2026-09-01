# CSpace — Capability-Based Access Control Architecture (v0.4.1)

**Status:** IMPLEMENTED (2026-08-16) — iteration-1 CSpace core, lifecycle
primitives, SYS_CAP_* syscalls, capability-gated IPC/frame mapping AND the
Untyped memory allocator + `cap::retype` (§2.8) are landed and green.
IRQ/MMIO caps remain deferred to v0.4.2+ (see §2.6).
**Build:** v0.4.1-dev (post v0.4.0 release)
**Owner:** kernel core (scheduler/IPC/memory/syscall)
**ROADMAP source:** §"Active Development — v0.4.1" + §Phase 4.6 (0.4.1 items 1–3)

---

## 1. Current State Assessment

### 1.1 Already exists (integration points)

| Capability prerequisite | Location | Notes |
|---|---|---|
| Intrusive shared-refcount base | `src/kernel/memory/kernel_object.hpp:60-142` | `acquire()` (:84, refused after revoke), `release()` (:96, 1→0 invokes `dispose()`), virtual `revoke()` (:74), virtual `dispose()` (:67), `is_shared()` (:116), `mark_pool_backed()` (:121). Atomic RELAXED acquire / ACQ_REL release. **v0.4.1 work item 1 is DONE — confirmed.** |
| RAII pin | `src/kernel/memory/kernel_object.hpp:157-177` | `ScopedRef` acquires on construction, no-op on revoked object. |
| Per-task intrusive object list | `src/kernel/task/task.hpp:398-399` (`task_obj_head_`/`tail_`), API :564-583 | `attach_object`/`detach_object`/`detach_all_objects`/`release_all_objects`; impl `src/kernel/task/task.cpp:138-229`. Bound: `CONFIG_MAX_PER_TASK_OBJECTS = 32` (`src/kernel/nexios_config.h:490-491`). Snapshot restore unlinks without release (poison-guard at task.cpp:183-187). |
| Shared-heap KernelObject pattern | `src/kernel/vfs/pipe.cpp:49-71` (`PipeBuffer`) | `dispose()` → `ResourceTracker::track_pipe_buffer_remove()` + `MemPool::free(this)`; `is_shared()` returns true. This is the template for capability objects. |
| Private-owned KernelObject pattern | `src/kernel/task/sporadic_server.hpp:45` | TCB-owned, list is single source of truth; `sporadic_server` read-cache kept in sync at task.cpp:130-131 / cleared at detach :164-165. Template for the TCB `cspace_` read-cache. |
| Syscall dispatch | `src/kernel/syscall/syscall.hpp:31-84` (`SyscallNumber` 0..50, `MAX_SYSCALL=51`), constexpr `syscall_table_` :216-269; `handle()` `src/kernel/syscall/syscall.cpp:94-135` | Handlers split across `syscall_handlers_{ipc,fs,process,sync,misc}.cpp`. ROADMAP §Syscall ABI requires the versioned table extension (0–50 → `SYS_CAP_*`). |
| IPC substrate | `src/kernel/ipc/ipc.hpp`; embedded per-TCB `MessageQueue` `src/kernel/task/task.hpp:63-101` | Today IPC is task-ID addressed (ambient authority). `IPC::send/recv/send_sync` by `dest_id`. |
| PMM (frame target) | `src/kernel/memory/pmm.hpp:42-120+` | `alloc_page/alloc_user_page/free_page/is_user_page`, contiguous variants. |
| MemPool | `src/kernel/memory/mempool.hpp` | Pool-backed objects freed via `MemPool::free` (see PipeBuffer). |
| ResourceTracker | `src/kernel/test/resource_tracker.hpp:29-41` counters, track_* :52-131; `check()` impl `resource_tracker.cpp:78-224` | Existing counters: mempool[9], pmm, tasks, bufpool, msg_queues, notifies, event_groups, drivers, pipe_buffers, vnodes, open_fds. **No capability counters — to add.** |
| Test isolation | `src/kernel/test/test_isolate.cpp:604-~720` | `snapshot_restore` runs under `IrqGuard`, drains zombies, then `ResourceTracker::check(baseline)` (:703) + `restore(baseline)` (:707). Every test must be leak-free at its end. |
| Test registry | `src/kernel/test/test_registry.cpp:199-441` class table; `register_all_tests()` :444-581; counts `src/kernel/test/test_expected_counts.hpp:18-192` (`all` = 886 at :23) | New test .cpp files auto-discovered (`mk/rules.mk:15` find), auto-scanned (`tools/gen_test_registry.py`), registered by `register_<file>_tests()` forward-declared in test_registry.cpp. `register_class` validates counts (:736). |
| Driven-test style | `src/kernel/test/test_pml4_clone.cpp`; helpers `src/kernel/test/test_sched_helpers.hpp` | Real tasks (`TaskControlBlock::create` + `Scheduler::add_task` + `reschedule` + `wait_for_termination_safe`), real `Syscall::handle` dispatch, `terminate_and_drain*`, `create_test_task`. |
| Global state accessors | `src/kernel/core/global_state.{hpp,cpp}` (NOTE: under `core/`, not `memory/` as the brief stated) | Per-call verified setters. **No new cross-TU globals are needed for iteration-1 CSpace** (capability roots are per-task, kept on the object list) — avoid extending `gs::` unless a registry is added later. |

### 1.2 Genuinely missing for CSpace

1. **CNode / CSlot / CSpace data structures** — no capability tables exist anywhere.
2. **Capability object KernelObject type** — nothing derives from `KernelObject` to represent a capability table.
3. **Endpoint kernel object** — IPC is task-ID ambient; no heap endpoint object a capability can reference.
4. **Frame kernel object** — PMM pages are ambient; no refcounted frame wrapper.
5. **`SYS_CAP_*` syscall numbers/handlers** — enum ends at `HALT=50`; table has no cap entries.
6. **Capability address translation** — no handle encoding/decoding, no slot generation counter.
7. **ResourceTracker counters** for capability objects/slots.
8. **Untyped allocator (seL4 retype)** — deferred (see §2.6).
9. **Capability references in `docs/specs/configuration.md`** — none exist; add a row when config tunables land (§2.7).

---

## 2. Design

### 2.1 Scope of this iteration (minimal first slice)

In-scope capability types: **CapTask**, **CapEndpoint**, **CapFrame**, **CapCNode** (implicit — the CNode is itself a capability for grant/copy addressing). Rights are a minimal bitmap checked at use time.

Explicitly deferred (design sketched in §2.6, landing v0.4.2+ per ROADMAP): Untyped memory + retype, IRQ caps, MMIO caps, multi-level (seL4 2^N-slot) CNodes, badge-based MINT semantics beyond a rights mask. The syscall surface in this iteration is exactly `SYS_CAP_GRANT/COPY/REVOKE/MINT` per ROADMAP 0.4.1; endpoint/frame **creation** stays a kernel-internal API (tests call it directly) until user-space ABI work (0.5.x).

### 2.2 Structures

New module: `src/kernel/cap/`.

```cpp
namespace kernel::cap {

enum class CapType : uint8_t {
    Null = 0, Task, Endpoint, Frame, CNode,
};

// Rights bitmap (checked by the operation that consumes the cap).
enum CapRights : uint32_t {
    CAP_RIGHT_READ   = 1u << 0,  // endpoint recv / frame read-mapping
    CAP_RIGHT_WRITE  = 1u << 1,  // endpoint send / frame write-mapping
    CAP_RIGHT_GRANT  = 1u << 2,  // may grant (copy into another CSpace)
    CAP_RIGHT_COPY   = 1u << 3,  // may copy (duplicate into own CSpace)
};

// One slot in a CNode: a strong reference (acquire()) to the target.
struct CSlot {
    KernelObject *obj = nullptr; // target; null when unoccupied
    CapType       type = CapType::Null;
    uint32_t      rights = 0;
    uint32_t      gen = 0;       // slot generation; stale-handle detection
    bool          occupied = false;
};

// Capability node: a table of slots. Itself a KernelObject (shared heap).
class CNode : public KernelObject {
  public:
    CSlot slots[CONFIG_CSLOT_COUNT];   // fixed table, bounded
    uint32_t cspace_id = 0;            // decoded from cap handles
    CNode *parent_ = nullptr;          // creator CNode (derivation tracking)
    uint32_t depth_ = 0;               // cascade-revoke depth bound
    sync::SpinLock lock_;              // serializes slot-table mutation
    void dispose() noexcept override;  // release all slots; MemPool::free
    bool is_shared() const noexcept override { return true; }
    void revoke() noexcept override;   // cascade: revoke+release children
};

// CSpace (iteration 1) == the task's root CNode:
//   TaskControlBlock::cspace_  (CNode* read-cache, sporadic_server pattern)
// The root CNode is attach_object()'d to the TCB so cleanup() →
// release_all_objects() → dispose() reclaims it deterministically.
```

Endpoint and frame objects follow the `PipeBuffer` shared-heap pattern:

```cpp
class Endpoint : public KernelObject {          // cap/endpoint.hpp/.cpp
  public:
    MessageQueue q;                             // existing embedded-queue type,
                                                // now heap-resident
    TaskControlBlock *bound_receiver = nullptr; // O(1) read cache, detached at
                                                // dispose; ownership NOT held
    uint32_t badge = 0;
    sync::SpinLock lock_;
    void dispose() noexcept override;           // detach receiver; tracker-;
                                                // MemPool::free
    bool is_shared() const noexcept override { return true; }
};

class FrameCap : public KernelObject {          // cap/frame.hpp/.cpp
  public:
    uint64_t phys = 0;       // first frame
    size_t count = 0;        // frame count (contiguous)
    bool is_user = false;    // PMM ownership class for free_page
    void dispose() noexcept override;           // PMM::free_page x count;
                                                // tracker-; MemPool::free
    bool is_shared() const noexcept override { return true; }
};
```

### 2.3 Capability address translation

- Handle = 64-bit opaque value: `(gen << (CBITS+IDBITS)) | (cspace_id << CBITS) | slot_index`, with `CBITS = ceil(log2(CONFIG_CSLOT_COUNT))`, `IDBITS = 8`.
- `cap::lookup(TaskControlBlock *cur, uint64_t handle, CapType want, uint32_t need_rights) -> KernelObject *` — validates against the **current task's own root CNode only** (no global registry in iteration 1; no ambient lookup). Checks: slot in range, `occupied`, `gen == slot.gen`, `type == want`, `(rights & need_rights) == need_rights`. Returns the target **already pinned** (the caller holds a `ScopedRef`); on any failure returns nullptr and the syscall returns -1.
- Slot recycling bumps `slot.gen`, so a stale handle for a reused slot fails decode deterministically.
- Lookup runs entirely in task context; the CNode `lock_` is held only for the slot-table read (short critical section).

### 2.4 Ownership / refcount model

- **One slot = one strong reference.** `cap_grant`/`cap_copy` call `target->acquire()` before installing the new slot; install is rollback-on-acquire-failure (revoked target → no slot).
- **Task → root CNode: exactly one reference**, held by the TCB object list (`attach_object`). Teardown: `cleanup()` → `release_all_objects()` drops it; last release runs `CNode::dispose()` which drains every slot (releasing each target once) and frees the block.
- **CNode is `is_shared()` true** so the teardown assert (`release_all_objects` : task.cpp:227-229) takes the `>= 1` path — pins from other tasks may legitimately extend lifetime.
- **Frame/endpoint lifetime is pinned by slots**; a `ScopedRef` holder outliving the last slot keeps the object alive until the pin drops (existing `KernelObject::release` 1→0 semantics).

### 2.5 Revocation semantics

- `SYS_CAP_REVOKE(handle)` → `cap::revoke(cspace, handle)`:
  1. Under `CNode::lock_`, validate + occupy-claim the slot (mark logically removed), **collect** the target into a local deferred-release list.
  2. Unlock. Then, outside the lock: if target is a `CNode`, **cascade** — recursively revoke its occupied slots (depth-bounded by `CONFIG_CAP_MAX_DEPTH`; iterative walk with an explicit work list, never unbounded recursion); otherwise call `target->revoke()` (marks `revoked_` RELEASE — future `acquire()` refused).
  3. Drop the slot reference(s): `target->release()` per collected slot. The 1→0 releaser runs `dispose()` — which may `MemPool::free` — **never under a spinlock** (avoids spinlock→mempool lock inversion).
- **Idempotency:** a second revoke on the same handle finds the slot unoccupied → no-op success.
- **Derivation:** `COPY` = new slot referencing the same object (ref bump, rights ∩ `CAP_RIGHT_COPY`); `GRANT` = COPY into a destination CNode (the destination must be addressed by a `CapCNode` handle owned by the caller; GRANT on a slot also clears the source slot's GRANT right after use — mint-once semantics); `MINT` = COPY with a rights-mask reduction and a new `badge` (endpoint badge only).
- **Cascade invalidation:** revoking a CNode revokes (marks) every object reachable through it — tasks lose endpoint/frame access immediately; the objects themselves are only freed when their last slot anywhere is released (reference-counted cleanup, deterministic).

### 2.6 Deferred design sketch (v0.4.2+)

- **Untyped memory (ROADMAP 0.4.1 item 3):** `UntypedMem : KernelObject` owning a contiguous PMM region; `retype(untyped_cap, Frame|CNode|Endpoint, ...)` carves one capability out of the region, transferring ownership; an Untyped cap may be retyped at most once (guard flag), and only the region's exact size may be carved. Requires a `CONFIG_CAP_MAX_UNTYPED` bound and a RegionAllocator (bump/bitmap over the Untyped range) — deferred with item 3 of 0.4.1, not blocking items 1–2.
- **IRQ caps (0.4.2):** `IrqCap : KernelObject` wrapping an IRQ vector → capability-backed `sys_irq_register`/`sys_irq_wait`.
  **IMPLEMENTED (2026-08-30, issue #2):** `CapType::Irq`, `src/kernel/cap/irq.{hpp,cpp}`
  (kernel-internal `create`; `CONFIG_CAP_MAX_IRQ`; single-owner per vector — a second live
  IrqCap for the same vector fails closed; x86_64 hardware IRQ window 33–47, timer vector 32
  reserved), the bounded delivery table `src/kernel/irq_delivery.{hpp,cpp}`
  (static `CONFIG_CAP_MAX_IRQ` slots; ISR entry from `handle_interrupt_c` BEFORE the threaded-IRQ
  path; EOI exactly once; pending IRQs never lost; blocked waiters woken by ISR/revoke/dispose —
  never left BLOCKED forever), and `SYS_IRQ_REGISTER` (57) / `SYS_IRQ_WAIT` (58) via
  `src/kernel/syscall/syscall_handlers_irq.cpp`.  Rights split: `sys_irq_register` requires
  WRITE (arming authority), `sys_irq_wait` requires READ (observe).  `IrqCap::dispose/revoke`
  and `TaskControlBlock::cleanup()` drain the delivery table (a dying task can never leave a
  dangling recipient/waiter); PIC mask state is captured at arm and restored at release.
  **Revocation limitation:** revoking/disposing an armed IrqCap wakes a blocked `sys_irq_wait`
  waiter with -1 (the wait contract holds); re-arming the same vector after teardown works
  (the slot is released).  aarch64/riscv64: handlers return -1 (no user-deliverable PIC vectors).
  **NOTIFY mode (2026-09-01, issue #7 — User-Space IRQ Delivery System):** `sys_irq_register`
  arg1 = delivery mode (`IrqDeliveryMode`: 0 = WAIT blocking `sys_irq_wait`, 1 = NOTIFY —
  IPC notification bridge).  In NOTIFY mode the ISR transforms the incoming interrupt into a
  notification on the recipient task's `Notify` object (value = vector, coalescing: last vector
  wins; drivers needing per-IRQ counts use WAIT mode), eliminating Ring 0 driver execution for
  notification-driven user-space drivers.  `sys_irq_wait` refuses NOTIFY-armed slots in both
  lock scopes.  The mode is bound atomically at arm under the slot lock (unknown modes fail
  closed).  Revoke/dispose/drain wake a blocked `sys_notify_wait` driver with the revoked
  sentinel 0 (wakers own the wakeup, CODING_STYLE §12.3); `drain_task` never calls notify on a
  dying task's already-destroyed Notify (cleanup destroys `~Notify` before `drain_task`).
  **EventGroup mode is deferred** to a follow-up: EventGroup has no error/value channel, so the
  revoke-wake contract would need a documented bit protocol, and its multi-waiter model mismatches
  the single-recipient delivery slot.  Class `cap_irq_notify` (7 tests); `all` registered 1003.
- **MMIO caps (0.4.2):** `MmioCap : KernelObject` wrapping a BAR range → capability-gated mapping (drives `sys_ioport_grant`).
  **IMPLEMENTED (2026-08-25, issue #3):** `CapType::Mmio`, `src/kernel/cap/mmio.{hpp,cpp}`
  (kernel-internal `create`/`create_from_bar`; `CONFIG_CAP_MAX_MMIO`; MEMORY ranges page-aligned,
  IO ranges confined to the 64 KiB port space), `VMM::map_mmio_from_cap`/`unmap_mmio_from_cap`
  (refuse revoked caps and IO-type ranges), and `SYS_IOPORT_GRANT` (55): capability-gated
  per-task TSS I/O bitmap delegation (x86_64, `arch::iopb_*`, static `CONFIG_IOPB_MAX_TASKS`
  pool, single global TSS with the 8 KiB bitmap inside `TSSBlock`, owner-memoized swap on user
  task switches, default-deny).  **Revocation limitation:** revoking an IO MmioCap blocks new
  grants but does NOT retroactively clear already-granted port bits in a live task's bitmap —
  task-bound grants die at task cleanup (derivation tracking is a follow-up).  aarch64/riscv64:
  handler returns -1 (no port I/O).
- **IOMMU DMA caps (0.4.2):** `IoMmuDmaCap : KernelObject` wrapping one private IOMMU DMA protection domain → capability-gated translation-table programming (`SYS_IOMMU_MAP`/`SYS_IOMMU_UNMAP`).
  **IMPLEMENTED (2026-08-30, issue #4):** `CapType::IoMmuDma`, `src/kernel/cap/iommu.{hpp,cpp}`
  (kernel-internal `create`; `CONFIG_CAP_MAX_IOMMU`; domain bound to the creating task — strict
  single-owner, a granted cap held by another task is refused), the static bounded table manager
  `src/kernel/iommu/iommu.{hpp,cpp}` (identity-IOVA second-level tables in the VT-d layout,
  `vtd.hpp`; per-domain SL root from zeroed PMM pages; cascade-empty-free; overlapping mappings
  rejected; revoked frames refused; leaf SpinLock, never held across cspace/dispose), and
  `SYS_IOMMU_MAP` (59) / `SYS_IOMMU_UNMAP` (60) via
  `src/kernel/syscall/syscall_handlers_iommu.cpp` (WRITE = arming authority; SL flags flow from
  the FRAME SLOT's granted rights; absent IOMMU → graceful -1).  `IoMmuDmaCap::dispose/revoke`
  destroys the domain FIRST (fail-closed: authority loss removes ALL DMA access).  Design +
  phase-2 (live DMAR/GCMD/IOTLB, AMD-Vi/SMMU backends): `docs/specs/iommu.md`.  aarch64/riscv64:
  handlers return -1.
- **Multi-level CNodes (0.7.x):** CNode-of-CNodes; the handle decode gains a radix walk.

### 2.7 Configuration additions (`src/kernel/nexios_config.h`)

| Tuner | Default | Notes |
|---|---|---|
| `CONFIG_CSLOT_COUNT` | 64 | per-CNode slot count; powers of two preferred |
| `CONFIG_CAP_MAX_DEPTH` | 8 | cascade-revoke depth bound (iterative walk) |
| `CONFIG_CAP_MAX_CNODES` | 16 | boot-time global CNode budget (enforced by tracker; per-task bound already `CONFIG_MAX_PER_TASK_OBJECTS`) |
| `CONFIG_INCLUDE_SYS_CAP_*` | 1 | mirrors existing `CONFIG_INCLUDE_SYS_*` pattern; add to `CONFIG_SYSCALL_COUNT` |

Add a row in `docs/specs/configuration.md` §1 (new §1.8 "Capabilities") documenting these tunables.

---

## 2.8 Untyped Memory Allocator (v0.4.1)

**Status:** IMPLEMENTED (2026-08-16, `c0d54371`) — ROADMAP v0.4.1 item 3,
iteration-1 foundation (supersedes the §2.6 deferred sketch for the Untyped
slice; IRQ/MMIO caps remain 0.4.2+).

**v0.4.2 extension — sub-range carve + child split (issue #1):** `cap::retype`
now carves a PAGE-aligned partial region and installs a **child Untyped** for
the remainder (exhaustion model — the child is itself retypable); added
**`SYS_CAP_RETYPE` (56)**. See §2.8.2 "Child Untyped for leftover".

### 2.8.1 Current state assessment

Already in place (integration points for Untyped):

| Piece | Location | Notes |
|---|---|---|
| KernelObject base | `src/kernel/memory/kernel_object.hpp` | refcount/revoke/dispose/`ScopedRef`; UntypedMem derives from this |
| CNode + CSlot | `src/kernel/cap/cap.{hpp,cpp}` | install/remove/peek/revoke/lookup/occupied_count; handle encode/decode; per-CNode SpinLock |
| FrameCap | `src/kernel/cap/frame.{hpp,cpp}` | owns PMM frames; dispose() → `PMM::free_page` × count → MemPool::free. Natural retype target |
| Endpoint | `src/kernel/cap/endpoint.{hpp,cpp}` | shared-heap object (NOT a retype target in iteration 1) |
| Lifecycle primitives | `src/kernel/cap/cap.cpp` | copy/grant/mint are type-agnostic (`CapType::Null` wildcard in lookup/do_copy_pinned); revoke cascades via `CNode::revoke` |
| SYS_CAP_* | `src/kernel/syscall/syscall.{hpp,syscall_handlers_cap.cpp}` | 51–54 (GRANT/COPY/REVOKE/MINT); NO retype syscall in iteration 1 |
| PMM | `src/kernel/memory/pmm.{hpp,cpp}` | `alloc_contiguous`/`alloc_user_contiguous`/`free_page`; every path calls `track_pmm_alloc/free` |
| ResourceTracker | `src/kernel/test/resource_tracker.{hpp,cpp}` | `cap_objects`/`cap_slots` counters; UntypedMem folds into `cap_objects` — no new counter |
| Config | `src/kernel/nexios_config.h:494-509` | `CONFIG_CSLOT_COUNT`=64, `CONFIG_CAP_MAX_DEPTH`=8; `CONFIG_CAP_MAX_UNTYPED` missing (`CONFIG_CAP_MAX_CNODES` from §2.7 never landed — do not add in this item) |

Genuinely missing for the Untyped slice:

1. **`UntypedMem` KernelObject** — owns a contiguous PMM region + retype-once guard.
2. **`retype()` operation** — carves a capability out of the region with ownership transfer.
3. **`CONFIG_CAP_MAX_UNTYPED`** — bound on live Untyped objects.
4. **`CapType::Untyped`** — add to `cap_types.hpp` (impact verified: `CapType` is only ever compared with `==`/`!=` — no exhaustive switch; `CapType::Null` remains the sole lookup wildcard at cap.cpp:177; copy/grant/mint duplicate the slot type verbatim, so Untyped caps become transferable with zero changes to those primitives).

### 2.8.2 Design

**UntypedMem object** — shared-heap KernelObject (PipeBuffer/CNode pattern):

```cpp
// src/kernel/cap/untyped.hpp
namespace kernel::cap {

/// Owns a contiguous PMM region and may be retyped at most once.
class UntypedMem : public KernelObject {
  public:
    uint64_t phys = 0;                        // first frame of the owned region
    size_t   size = 0;                        // region size in bytes (PAGE_SIZE multiple)
    bool     is_user = false;                 // PMM ownership class (FrameCap parity)
    CapType  retype_target = CapType::Frame;  // iteration-1: only Frame supported

    static UntypedMem *create(size_t size, bool is_user,
                              CapType target = CapType::Frame);
    /// Wraps an already-owned PAGE-aligned sub-range as a new Untyped WITHOUT
    /// allocating frames (the child remainder of a sub-range carve).  Enforces
    /// the shared CONFIG_CAP_MAX_UNTYPED live bound + cap-object tracking.
    static UntypedMem *create_subrange(uint64_t phys, size_t size,
                                       bool is_user,
                                       CapType target = CapType::Frame);

    /// CAS false->true; true iff this caller wins the region transfer.
    bool claim_once() noexcept;
    /// True iff this Untyped still owns its whole region (never retyped).
    bool owns_region() const noexcept;

    void dispose() noexcept override;  // frees frames ONLY if owns_region()
    bool is_shared() const noexcept override { return true; }
};

} // namespace kernel::cap
```

- `create()`: validates `size > 0 && size % arch::PAGE_SIZE == 0`; enforces `CONFIG_CAP_MAX_UNTYPED` via a TU-local live counter; carves the region from PMM (`is_user ? PMM::alloc_user_contiguous(n) : PMM::alloc_contiguous(n)`); allocates the block from MemPool, placement-news, `mark_pool_backed()`, `track_cap_object_add()`. On any failure after the PMM carve, the pages are returned to PMM before returning nullptr.
- `dispose()`: if `owns_region()` (never retyped) → `PMM::free_page(phys + i * arch::PAGE_SIZE)` for each frame; then `track_cap_object_remove()` + `MemPool::free(this)`. If retyped → frames are owned by the retyped object; dispose MUST NOT free them (double-free guard).
- The guard flag IS the ownership bit: `retyped_ == false` ⇒ Untyped owns the frames; `retyped_ == true` ⇒ Untyped owns nothing. Single flag, single source of truth.

**retype() operation** — kernel-internal API; `SYS_CAP_RETYPE` (56) dispatches
it on the caller's root CNode (v0.4.2, issue #1):

```cpp
/// Retypes the Untyped at @p untyped_handle in @p cspace into a new capability
/// of @p target_type, installed into @p cspace.  Supports sub-range carves
/// (PAGE-aligned, prefix-only): the remainder is installed as a child Untyped.
/// @return the new target slot index, or -1 on any failure.
int retype(CNode *cspace, uint64_t untyped_handle, CapType target_type,
           size_t size, uint32_t rights) noexcept;
```

Semantics (task context only; no IRQ context; no blocking):

1. `lookup(cspace, untyped_handle, CapType::Untyped, CAP_RIGHT_WRITE)` pins the Untyped (WRITE = mutation right; a dedicated `CAP_RIGHT_RETYPE` is a future extension).
2. Validate `target_type == ut->retype_target` (`CapType::Frame`), `size > 0`, PAGE-aligned, `size <= ut->size`. Any failure → release pin, -1. **The Untyped is left intact** on validation failure. `exact = (size == ut->size)`.
3. **Non-destructive slot-capacity pre-check**: a sub-range carve installs two slots (target + child), so `occupied_count(cspace) + (exact ? 1 : 2) <= CONFIG_CSLOT_COUNT` is required; failure → release pin, -1, parent intact.
4. **Claim the guard**: `ut->claim_once()` (CAS on `retyped_` false→true). Loser → release pin, -1. From this instant the Untyped owns nothing and no other retype can win.
5. `FrameCap *fc = FrameCap::create(ut->phys, size / arch::PAGE_SIZE, ut->is_user)`. On nullptr (MemPool exhaustion) → roll back the claim, release pin, -1. Region stays with the Untyped.
6. If `!exact`: `child = UntypedMem::create_subrange(ut->phys + size, ut->size - size, ...)`. On nullptr (MemPool / `CONFIG_CAP_MAX_UNTYPED` bound) → **stretch fail-closed**: set `fc->count = ut->size / arch::PAGE_SIZE`, release `fc` (whole region → PMM exactly once), release pin, -1. The parent stays spent (guard set).
7. `idx_target = cspace->install(fc, CapType::Frame, rights)`:
   - Success (and `!exact`) → `idx_child = cspace->install(child, CapType::Untyped, rights)`. On child-install failure → `cspace->remove(idx_target)` (rollback), release child (disposes remainder), release pin, -1.
   - Failure → release `fc` (disposes carved sub-range) and `child` if any (disposes remainder) — together the whole region returns to PMM exactly once; guard stays set (fail-closed); release pin, -1.
8. Success → drop creator refs (`fc->release()`, `child->release()` if any), release pin, return `idx_target`.

**Ownership/transfer invariant:** every frame in a region has exactly one owner
at every instant — Untyped (before claim) → in-flight (claim set) → exactly one
of {target FrameCap, child Untyped, PMM free-list} (after). PMM bitmap stays
consistent: the region is allocated exactly once (at `UntypedMem::create`) and
every page is freed exactly once by whichever owner disposes.

**RegionAllocator — NOT needed.** The carve is prefix-only: the remainder is a
single contiguous child range, so no interior free-list bookkeeping is
required.  Arbitrary interior carves are expressible by chaining child
retypes.  `UntypedMem::phys`/`size` are exactly the range a future
RegionAllocator would manage.

**Child Untyped for leftover — IMPLEMENTED (v0.4.2, issue #1).**  The §2.8.2
iteration-1 "exact-size carve only / retype at most once" limitation is lifted:
the retype guard now means "this Untyped's region has been transferred" (the
exhaustion model), and a sub-range carve installs a child Untyped for the
remainder.  The child is a full Untyped (owns its sub-range, itself retypable),
so a region can be split repeatedly.  Both child and parent dispose free
exactly their own sub-ranges; no frame is owned twice or by nobody.

**Retype targets — Frame only in iteration 1.** CNode and Endpoint are MemPool-allocated shared-heap structs, not physical-memory-resident objects; retyping a region "into" them requires object-storage-in-region support (deferred to 0.4.2). `retype_target` is the documented extension point.

**Revoke/dispose interplay:**
- Revoking a CNode cascades to its Untyped slot: `slot.obj->revoke()` marks the Untyped revoked; future `acquire()` fails. The Untyped still owns its region until the last reference drops → dispose frees it. Revoke invalidates the cap, not the memory.
- Retyped Untyped revoked later: dispose (guard set) frees only the MemPool block — no double-free.
- The retype-once guard is the only place the "who frees the region" decision is made; there is no path where both the Untyped and the FrameCap free the same frames.

**Config:** `CONFIG_CAP_MAX_UNTYPED` (default 16) in `nexios_config.h` cap section (~line 509). Enforced in `UntypedMem::create()` via the TU-local live counter.

### 2.8.3 Phased execution plan

Per AGENTS.md: `make build` clean between phases; one class at a time; `test-history.txt` row after every run.

**Phase 1 — UntypedMem object + CapType + config + object tests (3 tests)**
- MODIFY `src/kernel/cap/cap_types.hpp` — append `Untyped = 5` to `CapType`.
- MODIFY `src/kernel/nexios_config.h` — add `CONFIG_CAP_MAX_UNTYPED` (default 16).
- NEW `src/kernel/cap/untyped.hpp` / `untyped.cpp` — `UntypedMem` per §2.8.2.
- MODIFY `src/kernel/test/test_registry.cpp` — register `cap_untyped` class (both halves).
- MODIFY `src/kernel/test/test_expected_counts.hpp` — `{"cap_untyped", 3, 0, 0}`; `all` 932 → **935**.
- NEW `src/kernel/test/test_cap_untyped.cpp` — tests 1–3.
- Verify: `make execute-test x86_64 debug cap_untyped` → 3/3.

**Phase 2 — retype() operation + end-to-end tests (3 tests)**
- MODIFY `src/kernel/cap/cap.hpp` — declare `int retype(...)`.
- MODIFY `src/kernel/cap/untyped.cpp` — implement `cap::retype` (Frame-only, exact-size, CAS-guard transfer).
- Tests 4–6; `cap_untyped` 3→6; `all` 935→**938**.
- Verify: 6/6.

**Phase 3 — hardening tests + full gates (3 tests)**
- Tests 7–9; `cap_untyped` 6→9; `all` 938→**941**.
- Verify: 9/9 + regressions (cap_core 10, cap_lifecycle 8, cap_syscall 8, cap_ipc 6, memory_pmm 5) + debug `all` **941/941** (trace ON) + release `all` 84/84 (trace OFF) + selftest 132/132.
- Follow-up docs: ROADMAP.md item-3 checkbox; cspace.md status header amended.

**Phase 4 — sub-range carve + child split + SYS_CAP_RETYPE (v0.4.2, issue #1; +9 tests)**
- MODIFY `src/kernel/cap/untyped.{hpp,cpp}` — `create_subrange()`; retype() sub-range carve (prefix-only, exhaustion model, stretch fail-closed); shared `g_live_untypeds` bound counts children.
- MODIFY `src/kernel/cap/cap.hpp` — retype() doc (sub-range + child).
- MODIFY `src/kernel/syscall/syscall.hpp` + `syscall_handlers_cap.cpp` — `CAP_RETYPE = 56`, `MAX_SYSCALL = 57`, `sys_cap_retype` on the caller's root CNode.
- MODIFY `src/kernel/test/test_cap_untyped.cpp` — `retype_wrong_size_fails_keeps_untyped` inverted to `retype_oversize_rejected_parent_intact`; `retype_frame_exact_size_end_to_end` +occupied-count assert; 9 new tests (carve+child, two-level split, unaligned rejected, child-dispose-frees-remainder, parent-dispose-frees-nothing, full-table precheck, syscall dispatch + validation matrix, live-bound counts children).
- MODIFY `src/kernel/test/test_expected_counts.hpp` — `cap_untyped` 9→**18**; `all` 957→**966**.
- Verify: `cap_untyped` 18/18 + regressions (cap_core 10, cap_lifecycle 8, cap_syscall 8, cap_mmio 10, cap_ipc 6) + debug `all` **966/966** (trace ON) + release `all` 84/84 (trace OFF) + selftest.

### 2.8.4 SIL 3 considerations

1. **Double-free (primary risk).** The retype-once guard `retyped_` is the single ownership bit: `dispose()` frees the region iff `owns_region()`; after a successful retype the Untyped frees nothing and the FrameCap frees the same region once. No path frees twice: claim is CAS-single-winner; install failure frees via FrameCap::dispose with the guard left set (fail-closed); validation failure never touches ownership. The transient in-flight state (claim set, no FrameCap yet) is invisible to dispose because retype holds a pin (`lookup` acquire) — `dispose()` runs only on the 1→0 transition and cannot run while a pin exists.
2. **Concurrency boundaries.** retype is task-context only; no `IrqGuard`; no SpinLock added to UntypedMem — the lookup pin serializes lifetime against dispose; the CAS serializes retype-vs-retype. `retyped_` uses `__atomic_*` ACQ_REL / ACQUIRE. `FrameCap::dispose`/`UntypedMem::dispose` may `MemPool::free` — never under a cap spinlock.
3. **Refcount/revoke interaction.** Retyped or not, the Untyped is an ordinary shared KernelObject: revoke marks it revoked (future acquire refused) but memory reclaim happens on the last release; a pin taken before revoke completes its operation safely. The pre-existing `acquire()` overflow-guard gap (spec §4.2; kernel_object.hpp:84-88 lacks the UINT32_MAX check) is out of scope here.
4. **Bounded structures.** `CONFIG_CAP_MAX_UNTYPED` (16) enforced by the TU-local live counter; region size bounded by PMM + page-multiple validation; no unbounded recursion (retype is O(1) plus FrameCap::create/install).
5. **Memory-safety of the carve.** `size % arch::PAGE_SIZE == 0` and `size > 0` validated at create; `phys` is contiguous by construction; retype validates `size == ut->size` so the FrameCap never wraps a different range; both disposes iterate `count = size / arch::PAGE_SIZE` frames from the same base.
6. **ResourceTracker.** UntypedMem folds into the existing `cap_objects` counter; the region is tracked by the existing `pmm_pages_used` counter. No new counters; snapshot_restore's `check()` fails any test leaking Untyped objects or frames.

### 2.8.5 Gate / Definition of Done

- [ ] `make build` green after every phase.
- [ ] Phase gates: `cap_untyped` 3/3 → 6/6 → 9/9, zero ResourceTracker delta each.
- [ ] Regression: `cap_core` 10/10, `cap_lifecycle` 8/8, `cap_syscall` 8/8, `cap_ipc` 6/6, `memory_pmm` 5/5.
- [ ] Debug `all` **941/941** (trace ON), release `all` **84/84** (trace OFF), `selftest` 132/132.
- [ ] `test-history.txt` rows appended for every gate run.
- [ ] ROADMAP.md item-3 checkbox checked; cspace.md status header amended.
- [ ] SIL 3 audit (auditor subagent) approves every modified file; diff-patch protocol for any REJECT.

**Counts summary:** +9 tests (class `cap_untyped`) → `all` 932 → **941**. Release/safe counts unchanged (84/132) — cap_untyped tests are TF_KERNEL.

**v0.4.2 (issue #1) counts:** `cap_untyped` 9→**18**, `all` 941→**966**. Release/safe counts unchanged — the new carve/syscall tests are TF_KERNEL. SIL 3 audit evidence: `audits/report-<issue-#1>` (DECISION: APPROVED).

---

## 3. Phased Execution Plan

Each phase ends with a green class gate. Fix classes one at a time; per-class discipline, `make build` clean, `test-history.txt` row after every run (AGENTS.md rules).

### Phase 1 — CSpace core engine (CNode/CSlot/handle decode)

- **Scope:** data structures, handle encoding/decoding, slot install/remove under per-CNode spinlock, root-CNode lifecycle, ResourceTracker counters.
- **Files:**
  - NEW `src/kernel/cap/cap_types.hpp` (CapType, CapRights, CSlot, handle helpers)
  - NEW `src/kernel/cap/cap.hpp` / `src/kernel/cap/cap.cpp` (CNode, lookup, slot ops, revoke core, deferred-release helper)
  - MODIFY `src/kernel/nexios_config.h` (§2.7 tunables)
  - MODIFY `src/kernel/task/task.hpp` (`CNode *cspace_;` + accessor near `sporadic_server` :384; forward-declare `cap::CNode`), `src/kernel/task/task.cpp` (create root CNode in `init_task_common` or lazily; keep `cspace_` read-cache in sync at attach/detach like task.cpp:130-131/:164-165)
  - MODIFY `src/kernel/test/resource_tracker.hpp` (:29-41 struct, new `cap_objects`/`cap_slots` counters + `track_cap_object_add/remove`, `track_cap_slot_add/remove`) and `src/kernel/test/resource_tracker.cpp` (`any_leak` :78-104, `print_row` :210-220)
  - MODIFY `src/kernel/test/test_registry.cpp` — forward-declare `register_cap_core_tests()`; add class row later in Phase 5 (or now with a 0-count placeholder? No — add in Phase 5 with counts).
- **Syscalls:** none.
- **Tests:** NEW `src/kernel/test/test_cap_core.cpp` — class **`cap_core`**, 10 tests:
  `cslot_init_empty`, `cnode_install_acquires_ref`, `cnode_release_drops_ref`, `cnode_dispose_frees_pool_block`, `cnode_revoke_refuses_acquire`, `cnode_cascade_revoke_marks_children`, `handle_decode_valid`, `handle_decode_out_of_range_fails`, `handle_decode_stale_gen_fails`, `root_cnode_attached_to_task_teardown_frees`.
  Style: driven tests — real tasks via `TaskControlBlock::create` + `Scheduler::add_task` + `wait_for_termination_safe` where tasks are involved; stack-allocated CNode only where allowed (never pool-marked), otherwise `mark_pool_backed` + `MemPool` alloc/free balanced; every test ends at ResourceTracker baseline.
- **Verify:** `make execute-test x86_64 debug cap_core` → 10/10, 0 failures, zero ResourceTracker delta.

### Phase 2 — Lifecycle primitives + Endpoint/Frame objects

- **Scope:** `cap_grant/cap_copy/cap_revoke/cap_mint` internal API; `Endpoint` and `FrameCap` objects; deterministic cleanup on last release.
- **Files:**
  - NEW `src/kernel/cap/endpoint.hpp` / `src/kernel/cap/endpoint.cpp` (`Endpoint`; heap-resident `MessageQueue` reuse)
  - NEW `src/kernel/cap/frame.hpp` / `src/kernel/cap/frame.cpp` (`FrameCap`; PMM free on dispose)
  - MODIFY `src/kernel/cap/cap.cpp` — `cap_grant`, `cap_copy`, `cap_revoke`, `cap_mint` (internal, task-context only; destination CNode addressed by `CapCNode` handle)
  - MODIFY `src/kernel/test/resource_tracker.{hpp,cpp}` — `cap_endpoints`, `cap_frames` counters (or fold into `cap_objects`; **recommended: fold into `cap_objects` + `cap_slots`** to keep surface minimal — choose one, document it)
- **Syscalls:** none (creation is kernel-internal API in this iteration).
- **Tests:** NEW `src/kernel/test/test_cap_lifecycle.cpp` — class **`cap_lifecycle`**, 8 tests:
  `grant_creates_slot_in_dest`, `copy_shares_refcount`, `revoke_removes_slot_and_releases`, `revoke_twice_idempotent`, `mint_reduces_rights`, `revoke_while_scopedref_pinned_delays_dispose`, `frame_cap_release_frees_pmm`, `endpoint_cap_release_disposes`.
  Each test must terminate/drain every created task and free every pool object before returning (snapshot_restore checks).
- **Verify:** `make execute-test x86_64 debug cap_lifecycle` → 8/8.

### Phase 3 — SYS_CAP_* syscalls

- **Scope:** versioned syscall-table extension (0–50 → 51–54) + handlers.
- **Files:**
  - MODIFY `src/kernel/syscall/syscall.hpp` — append `SYS_CAP_GRANT = 51`, `SYS_CAP_COPY = 52`, `SYS_CAP_REVOKE = 53`, `SYS_CAP_MINT = 54`, `MAX_SYSCALL = 55`; declare 4 handlers; append 4 entries to `syscall_table_` (:216-269); add an ABI-version comment per ROADMAP §Syscall ABI.
  - NEW `src/kernel/syscall/syscall_handlers_cap.cpp` — `sys_cap_grant/copy/revoke/mint`: decode handle against `syscall_task()`'s root CNode, enforce rights, return 0/-1; all dereferences through `ScopedRef`; no blocking.
  - MODIFY `src/kernel/nexios_config.h` (`CONFIG_INCLUDE_SYS_CAP_*`, add to `CONFIG_SYSCALL_COUNT`).
- **Tests:** NEW `src/kernel/test/test_cap_syscall.cpp` — class **`cap_syscall`**, 8 tests, **real `Syscall::handle()` dispatch** (mirror `test_syscall.cpp`):
  `sys_cap_grant_dispatch`, `sys_cap_copy_dispatch`, `sys_cap_revoke_dispatch`, `sys_cap_mint_dispatch`, `sys_cap_bad_handle_returns_minus1`, `sys_cap_wrong_type_returns_minus1`, `sys_cap_rights_denied_returns_minus1`, `sys_cap_revoke_cleanup_zero_delta`.
- **Verify:** `make execute-test x86_64 debug cap_syscall` → 8/8.

### Phase 4 — Capability-gated IPC + frame mapping (integration)

- **Scope:** CapEndpoint-mediated send/recv; CapFrame-mediated page mapping; the new path removes ambient authority for its consumers (legacy task-ID IPC remains for compatibility, documented as deprecated).
- **Files:**
  - MODIFY `src/kernel/ipc/ipc.hpp` / `ipc.cpp` — add `IPC::send_via_cap(cap::Endpoint*, const Message&, uint64_t flags)` and `IPC::recv_via_cap(cap::Endpoint*, Message&)`; reuse `block_sender`/`wake_sender` primitives; receiver binding honors `Endpoint::bound_receiver` under `Endpoint::lock_`.
  - MODIFY `src/kernel/memory/vmm.hpp` / `vmm.cpp` — add `VMM::map_frame_from_cap(FrameCap*, va, flags, pml4)` (validates rights + `FrameCap::phys`), `VMM::unmap_frame_from_cap(...)`; revocation path unmaps via a per-frame mapping registry **only if one is introduced** — otherwise document that frame unmapping is the holder's duty and enforce it at task cleanup (walk object list).
- **Tests:** NEW `src/kernel/test/test_cap_ipc.cpp` — class **`cap_ipc`**, 6 tests:
  `endpoint_send_recv_roundtrip` (real tasks, real blocking — driven style), `endpoint_revoked_send_fails`, `frame_cap_map_unmap_roundtrip`, `frame_cap_revoke_denies_map`, `send_without_endpoint_cap_denied`, `endpoint_teardown_no_leak`.
- **Verify:** `make execute-test x86_64 debug cap_ipc` → 6/6.

### Phase 5 — Registry wiring, counts, full gates

- **Scope:** register the four classes, update counts, run full gates.
- **Files:**
  - MODIFY `src/kernel/test/test_registry.cpp` — forward-declare `register_cap_core_tests`, `register_cap_lifecycle_tests`, `register_cap_syscall_tests`, `register_cap_ipc_tests`; add class rows `cap_core`/`cap_lifecycle`/`cap_syscall`/`cap_ipc` in `g_test_classes`; add the four `register_*` calls to `register_all_tests()` (:444-581) and (if the ~850-cycle snapshot budget is a concern) to the halves.
  - MODIFY `src/kernel/test/test_expected_counts.hpp` — add 4 rows:
    `{"cap_core", 10, 0, 0}`, `{"cap_lifecycle", 8, 0, 0}`, `{"cap_syscall", 8, 0, 0}`, `{"cap_ipc", 6, 0, 0}`; bump `all` 886 → **918** (adjust to `dump_class_counts` actual if it differs — the harness prints `[TCOUNT]` mismatches).
- **Verify sequence (each with a `test-history.txt` row):**
  1. `make execute-test x86_64 debug cap_core` → 10/10
  2. `make execute-test x86_64 debug cap_lifecycle` → 8/8
  3. `make execute-test x86_64 debug cap_syscall` → 8/8
  4. `make execute-test x86_64 debug cap_ipc` → 6/6
  5. `make execute-test x86_64 debug all` → **918/918** (trace ON per debug-gate rule)
  6. `make execute-test x86_64 release all` → 84/84 (trace OFF — verify `CONFIG_DEBUG_IPC_SCHED` undefined first)
  7. `make execute-test x86_64 debug selftest` → 132/132 (cap classes stay TF_KERNEL; safe class unchanged unless release-marked tests are deliberately added)
- **Regression:** re-run the touched subsystem classes (`ipc_core`, `memory_vmm`, `process_pml4_clone`, `syscall_core`) to prove no collateral.

---

## 4. SIL 3 Considerations

1. **Concurrency boundaries**
   - Cap slot-table mutation is **task-context only**, serialized by the per-CNode `sync::SpinLock` (short critical sections; SpinLockGuard). Per ROADMAP guardrails: no `IrqGuard` in cap paths (reserved for boot/panic/test isolation). ISR context never touches CNode/CSlot; `acquire()`/`revoked()` remain the only cross-context surface and are already atomic (ACQUIRE/RELEASE, kernel_object.hpp:84-88).
   - **Release-after-unlock discipline:** `dispose()` may call `MemPool::free` (takes mempool locks) and `ResourceTracker` — it must never run while holding a CNode/Endpoint spinlock. `revoke` collects targets under the lock and releases after unlock (explicit deferred-release work list).
2. **Memory safety**
   - **Double-free:** slot release only when `occupied`; revoke idempotent (slot cleared on first pass); `slot.gen` invalidation defeats stale-handle reuse of recycled slots; teardown order in `release_all_objects` unlinks before releasing (task.cpp:212-229) — never free a still-linked CNode.
   - **Refcount overflow:** add a guard in `KernelObject::acquire()` — refuse when `ref_count_ == UINT32_MAX` (fail-closed; prevents wraparound to a live double-free). Release-side 1→0 is already the sole `dispose()` trigger.
   - **Bounded structures:** `CONFIG_CSLOT_COUNT`, `CONFIG_CAP_MAX_DEPTH` (iterative cascade walk — no unbounded recursion / stack overflow), `CONFIG_MAX_PER_TASK_OBJECTS` (already bounds the TCB list walk, task.cpp:176). Poison-guard compatibility: `detach_all_objects` stops at 0xDD-poisoned blocks (task.cpp:183-187) — CNode must never be freed while still linked.
3. **Revocation races**
   - `revoked_` publication uses RELEASE (revoke) / ACQUIRE (acquire) — a revoke on another CPU is visible before any subsequent acquire decision (already in kernel_object.hpp:74-88).
   - Every dereference of a capability target happens only through a successful `acquire()` (ScopedRef). A pinned holder that started before revoke finishes its operation safely; the object dies on the final release.
   - TCB teardown assert: CNode/Endpoint/FrameCap are `is_shared() == true` → `>= 1` path (task.cpp:227-229), so legitimate cross-task pins do not trip the debug assert.
4. **ResourceTracker**
   - New counters (`cap_objects`, `cap_slots`, or folded set) must be added to `ResourceCounters`, `track_*` methods, `any_leak` (:78-104) and the `print_row` block (:210-220); every pool alloc/free and slot install/remove is paired. snapshot_restore's `check()` (:703) then fails any test leaking cap objects — the primary leak gate.
   - Do not rely on `restore(baseline)` (:707) to hide leaks — tests must clean up before returning.
5. **Determinism**
   - No dynamic allocation in cap syscall paths beyond the bounded slot/CNode tables; no blocking while holding a cap spinlock; cascade revoke is O(CONFIG_CSLOT_COUNT × depth) worst case, bounded and documented for WCET review.

---

## 5. Gate / Definition of Done — v0.4.1 CSpace milestone

- [ ] `make build` green (`check-style debug`: 0 errors).
- [ ] Debug class gates: `cap_core` 10/10, `cap_lifecycle` 8/8, `cap_syscall` 8/8, `cap_ipc` 6/6 — all with zero ResourceTracker delta.
- [ ] Debug `all` **918/918** (trace ON), release `all` **84/84** (trace OFF — verify `CONFIG_DEBUG_IPC_SCHED` undefined), `selftest` 132/132.
- [ ] `test-history.txt` rows appended for every gate run.
- [ ] `docs/specs/configuration.md` §1.8 documents the new tunables; `docs/specs/cspace.md` updated to "IMPLEMENTED" with the final structure/fields and any deviations.
- [ ] ROADMAP §0.4.1 items 1–2 checked; item 3 (Untyped) explicitly deferred to v0.4.2+ with the design sketch in §2.6 (no silent drop).
- [ ] SIL 3 audit (auditor subagent) approves every modified file; diff-patch protocol for any REJECT.
- [ ] ROADMAP §"Version guide" note: capability-complete security at v1.0.0 still requires IRQ/MMIO/Untyped caps (0.4.2+) — this milestone is the foundation, not the completion, of ambient-authority elimination.

**Counts summary:** +32 tests (10+8+8+6) → `all` 886 → **918**. Release/safe counts unchanged (84/132) unless release-marked cap smoke tests are deliberately added in Phase 5.

---

## 6. Implementation Log (v0.4.1)

All five phases landed 2026-08-15. Final gates: debug `all` 905/905 executed
(920 registered), release `all` 84/84, selftest 132/132, `make check-style`
0 errors.

- **Phase 1** (`f58be694`) — `src/kernel/cap/cap_types.{hpp}`, `cap.{hpp,cpp}`:
  `CNode` (shared-heap KernelObject), `install`/`remove`/`peek`/`slot_gen`/
  `clear_grant`, handle encode/decode, `lookup()` (pins via acquire, treats
  `CapType::Null` as a type wildcard), `revoke()` (iterative cascade,
  depth-bounded by `CONFIG_CAP_MAX_DEPTH`), `occupied_count`.  TCB gains
  `cspace_` root-CNode read cache + `ensure_cspace()` (lazy, attached to the
  intrusive object list).  `nexios_config.h`: `CONFIG_CSLOT_COUNT`=64,
  `CONFIG_CAP_MAX_DEPTH`=8.  ResourceTracker: `cap_objects`/`cap_slots`.
  Class `cap_core` (10 tests).
- **Phase 2** (`f1270468`) — `endpoint.{hpp,cpp}`, `frame.{hpp,cpp}`:
  `Endpoint` (heap-resident `MessageQueue` + `bound_receiver` + `badge`),
  `FrameCap` (owns PMM frames, `dispose()` frees them).  `cap::copy/grant/
  mint` lifecycle primitives (grant consumes source GRANT mint-once; mint
  reduces rights; copy never widens).  `CNode::dispose()` guards
  `MemPool::free` with `is_pool_backed()`.  Class `cap_lifecycle` (8 tests).
- **Phase 3** (`ec710192`) — `SYS_CAP_GRANT`=51, `SYS_CAP_COPY`=52,
  `SYS_CAP_REVOKE`=53, `SYS_CAP_MINT`=54, `MAX_SYSCALL`=55;
  `syscall_handlers_cap.cpp` (destination CNode addressed by a CapCNode
  handle in the caller's own CSpace; GRANT/COPY rights enforced; no
  blocking).  Class `cap_syscall` (8 tests).
- **Phase 4** (`ed6c57a1`) — `IPC::send_via_cap`/`recv_via_cap` (endpoint
  transport, refuses a revoked endpoint, wakes bound receiver, blocks on
  full queue), `VMM::map_frame_from_cap`/`unmap_frame_from_cap` (refuses a
  revoked cap).  Class `cap_ipc` (6 tests).
- **Phase 5** — registry rows + counts (`all` 920), full gates above.
- **Phase 6** (`c0d54371`) — Untyped memory allocator (ROADMAP 0.4.1 item 3):
  `UntypedMem` (contiguous PMM region, retype-once guard = single ownership
  bit), `cap::retype` (Frame-only, exact-size carve, CAS-claimed transfer),
  `CapType::Untyped`, `CONFIG_CAP_MAX_UNTYPED`.  Class `cap_untyped` (9
  tests); `all` 941.  Child-Untyped split deferred to v0.4.2 (§2.8).

**Deviations from the plan (all recorded):**
- `all` registered count is **920**, not the estimated 918 — the pre-CSpace
  baseline was 888 (the plan's 886 figure predated the last pre-release
  additions); executed count under the harness filter is 905.
- `CapType::Null` doubles as a lookup wildcard so grant/copy/mint can
  duplicate any capability type.
- The dest CNode for grant/copy/mint is addressed by a CapCNode handle in
  the caller's own CSpace (requires holding a CNode cap), not an ambient
  cspace id.
