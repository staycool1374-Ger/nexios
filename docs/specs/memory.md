# Memory Subsystem Specification (PMM / MemPool / VMM / Stacks / Snapshot)

**Semantics:** binding contracts for physical-memory management, the fixed
MemPool allocator, the VMM page-table layer, kernel/user stack provisioning,
guard pages, and the test snapshot-restore isolation mechanism.
Synthesis of `_archive/memory-subsystem-audit-fix.md`,
`_archive/memory-protection-spec.md`, `_archive/kstack-window-pt-pool.md`,
`_archive/stack-guard-spec.md`, `_archive/fork-pt-deep-copy.md`,
`_archive/hhdm-snapshot-restore.md`, and the MemPool root cause from
`_archive/investigation-cumulative-corruption.md`.  `[IMPLEMENTED]` =
code-verified.

## 1. Memory Layout

```
 phys 0                                  HHDM = 0xFFFF800000000000 (direct map)
  0x1000   PML4 (kernel)                 PML4[0]   = PDPT_IDENTITY (low 512 GB)
  0x2000   PDPT (identity)               PML4[256] = PDPT_HIGHER (kernel half)
  0x3000   PD (identity, huge 2 MiB)     PML4[1..255,257..511] zeroed
  ...                                    0xFFFF900000000000 = kslot window (16 MiB)
  ~4 MB    kernel image / data
  ...                                    0xFFFF8000xxxxxxxx = HHDM alias of RAM
```

- **PMM** manages the allocation bitmap (1 bit/page), the user/kernel owner
  bitmap (1 = USER), the O(1) free list, and the reserved page-table pool.
- **MemPool** manages fixed-size blocks; pool-2 = 64-byte blocks, 320 blocks.

### 1.1 Address constants
```
HHDM_OFFSET     = 0xFFFF800000000000   (kernel direct map of all RAM)
KERNEL_STACK_BASE = 0xFFFF900000000000 (private kstack VA window, 16 MiB)
USER_SPACE_LIMIT = 0x00007FFFFFFFFFFF (x86_64)
```

## 2. VMM / Page-Table Contract

- `map_page_in_pml4(va, phys, user, ...)` **must not** map a KERNEL-owned page
  user-accessible: `ENSURE(PMM::is_user_page(phys))` (VULN-004, IMPLEMENTED).
- All newly allocated page-table pages MUST use `PMM::alloc_user_page()` for
  user-space hierarchies so `VMM::free_user_pages()` can free them
  (fork spec, IMPLEMENTED).
- Huge-page splitting: 2 MiB → new PT + 512×4 KiB copies; 1 GiB → 512 PD
  entries (fork spec).
- **Remap hazard [OPEN]:** `map_page_in_pml4` unconditionally overwrites a
  present leaf PTE (vmm.cpp:558) and does not free the old USER page.  Mapping
  over an existing mapping orphans the old page (e.g. the yield-stub collision
  in `buffer_pool_exhaustion`; fixed in v0.3.11 by moving the buffer base VA,
  but the general remap-orphan remains a latent leak).

### 2.1 Fork deep copy [IMPLEMENTED]
`TaskControlBlock::clone()` → `VMM::deep_copy_user_pages()`: parent and child
get fully independent page-table trees (recursive walk, new PDPT/PD/PT + new
data pages).  `page_table_shared_ = false`.  COW deferred to a later version.

```
 parent PML4 ──clone──▶ child PML4
   ├─ user entries deep-copied (new PDPT/PD/PT/data pages)
   └─ kernel half shared read-only view
```

## 3. Kernel-Stack Provisioning & Guard Pages

### 3.1 kslot window [IMPLEMENTED]
- Production kernel tasks use the private VA window `0xFFFF900000000000`
  (16 MiB) via `alloc_kslot()/map_kstack_page()/free_kslot()`; test-mode tasks
  use HHDM (`!Scheduler::is_test_active()` gate).
- The window needs exactly **10 pre-allocated page-table pages** (1 PDPT + 1 PD
  + 8 PT; each PT covers 2 MiB, 8×2 = 16 MiB), provisioned from the boot
  page-table pool into `s_kstack_pt_pages[]` — NOT restored by snapshot_restore.
- Map/unmap write directly into `s_kstack_pt_pages[pt_idx]` with `invlpg`
  (x86_64; `tlbi vmalle1` on aarch64, `sfence.vma` on riscv64), never
  `PMM::alloc` per stack.

### 3.2 Slot layout & guard page
```
 slot_base = KERNEL_STACK_BASE + i*SLOT_SIZE
   [slot_base]              guard  (unmapped 4 KiB)
   [slot_base + PAGE_SIZE]  stack  (grows down)
   #PF on cr2 ∈ window  →  find_task_by_kstack_slot → panic("kernel stack overflow")
```
Size tiers `CONFIG_STACK_SIZE_TABLE { 4096, 4096, 16384, 16384, 32768, 32768, 65536, 65536 }`.
The guard-page approach was adopted; software canaries are a secondary layer
(`CONFIG_TCB_WRITE_LOG` ring-buffer write tracker exists as prior art).

### 3.3 User-stack guard
`create_user()` maps the user stack at `STACK_VADDR + PAGE_SIZE`, leaving
`STACK_VADDR` unmapped → guard page below every user stack.

## 4. MemPool Contract

- **VULN-001 FIXED [IMPLEMENTED]:** `freed_bitmap`/`pinned_bitmap` widened from
  `[4]` to `[BITMAP_WORDS]` where `MAX_BLOCKS_PER_POOL = 320`,
  `BITMAP_WORDS = (320+63)/64 = 5`.  (This was the cumulative-corruption root
  cause: `set_block_pinned(idx≥256)` wrote `pinned_bitmap[4..7]`, past the
  4-entry array, corrupting adjacent PoolMeta static storage.)
- **VULN-002 IMPLEMENTED:** `pmm_lock_`/`mempool_lock_` are `sync::SpinLock`
  (POD, `constinit`, no heap); all mutating entry points wrapped in
  `IrqSpinLockGuard`.  OOM handler callbacks release the lock before invoking
  the handler and re-acquire for retry.
- O(1) intrusive free-list (VULN-003) deferred.

## 5. PMM Owner-Bit & BufferPool Pool Contract (v0.3.11 lessons)

- Every physical page has an **owner bit** (1 = USER, 0 = KERNEL) in the owner
  bitmap.  `free_user_pages()` only frees USER-owned table pages and leaves.
- **BufferPool pool** (`POOL_PAGES = 128`, `pool_pages_[]`): a bounded USER
  cache for buffer data.  Contracts:
  1. `alloc_page()` pop and `free_page()` push MUST be slot-consistent:
     pop reads `pool_pages_[__atomic_sub_fetch(count,1)]` (= old-1), push MUST
     write `pool_pages_[__atomic_fetch_add(count,1)]` (= old).  The historical
     `__atomic_add_fetch` push stored at old+1 — an off-by-one that lost pushed
     pages to an unread slot and drifted the ResourceTracker +1
     (FIXED 2026-08-06).
  2. `pool_pages_[]` MUST be captured/restored by the snapshot
     (`capture_state/restore_state/state_bytes`), else snapshot_restore leaves
     the pool holding pages the rewound PMM bitmap has freed (+1..+128 drift).
  3. Buffer VAs MUST be ≥ `0x100000000` (documented convention) — VA
     `0x40000000` collides with the `kUserYieldStubVa` (task.cpp) and orphans
     the stub page on remap.

## 6. Snapshot / Restore Isolation Contract

**Semantics:** every kernel test runs from a clean rewindable baseline;
`snapshot_create()`/`snapshot_restore()` capture/restore a flat buffer covering
PMM bitmaps, MemPool data+meta, scheduler state, the ready-queue POD, task
fields, kstack contents, and BufferPool state.

Restore order (correct, from the HHDM UAF fix):
```
1. HHDM PD restore   (only when pdpt[0]==0x5000 boot PD; free split PT pages
                       into the live bitmap FIRST)
2. PMM bitmap + owner bitmap + free counter rewind
3. PtPoolSnapshot restore
4. scheduler state: tasks/id_table/misc + restore_task_fields (deep-copy TCBs)
5. rebuild_all_tasks + rebuild_ready_queue (heals all RQ desyncs)
6. re-identify current task by RSP ownership
7. BufferPool::restore_state (entries + pool_count + pool_pages_)
8. ResourceTracker::restore(baseline)
9. kernel-stack restore (skip the current task's stack)
```

Constraints:
- `map_page` kernel-space guard: test-mode modifications of kernel VAs
  (`pml4_idx >= PML4_USER_COUNT`) set `hhdm_modified_` so the PD restore runs.
- `restore_task_fields` matches TCBs by `id`; `kernel_stack` is NOT in
  `TaskFields` (a stray write zeroes it permanently) — see `specs/scheduler.md`
  gaps.

## 7. Memory-Protection Requirements (0.4.x, OPEN)

| Req | Semantics | Status |
|---|---|---|
| REQ-MP-01 | kernel↔user complete isolation (text/data/stack) | present |
| REQ-MP-02 | kernel-task↔kernel-task private kernel-half page tables | present (MP-1, `memory_kernel_isolation`) |
| REQ-MP-03 | user-task↔user-task isolation | present (deep-copy fork) |
| REQ-MP-04 | kernel→user access via direct map | present |
| REQ-MP-05 | user→data only via syscalls; SMAP/SMEP recommended-not-mandatory | present + HW-enforced (MP-4, x86_64: CR4.SMEP + CR4.SMAP; aarch64 PAN/PXN deferred) |
| REQ-MP-06 | per-task canary-protected segments (MMU + software sentinels) | partial (guard pages only) |

Acceptance (REQ-MP-06): "Overwriting a segment-boundary software canary is
detected at the next syscall/context-switch and causes a controlled panic."

## 7.1 Private kernel-half page tables (v0.4.0 MP-1 addendum)

Since MP-1 every task — kernel AND user — owns a private PML4 (`page_table_`)
whose kernel half (entries `>= PML4_KERNEL_START`, i.e. 256..511 on x86_64) is
copied **by value** from the boot kernel PML4 via `VMM::clone_kernel_pml4()`:

- **Kernel text/data/bss** map (higher-half kernel region): inherited by value.
- **HHDM direct map** (phys↔virt, all of RAM): inherited by value; kernel code
  that touches arbitrary user frames keeps working in every task context.
- **Shared kslot window** (`CONFIG_KSTACK_WINDOW_BASE`, PML4 index 498 >=
  `PML4_KERNEL_START`): the window's PDPT/PD/PT pages are wired once into the
  boot kernel PML4 at `init_kstack_window()`; every private clone inherits the
  PD/PT entries **by value**, so the window tables are shared by reference.
  Per-task private kstack page tables are NOT allocated.  Stack isolation is
  enforced by the guard page below each slot and by the MP-6 hook.
- **User half** (entries 0..255): zeroed in every kernel task's PML4; user
  tasks populate it via ELF load / `create_user()` / fork deep copy.

Consequences (authoritative discriminator change):
- `page_table_ != 0` no longer identifies a user task — use `is_user_`.
- `syscall_is_user_task()`, OOM-victim selection, exception/signal delivery and
  VFS IPC authorization all key on `is_user_`.
- `switch_to_task` publishes `scheduler_load_cr3_from = next.page_table_` for
  every task, so every context switch reloads CR3 in the same ISR-epilogue
  path; the CR3-clear paths (`clear_switch_globals`, deferred-switch CLR,
  `scheduler_validate_pending_switch`) zero it unconditionally.
- Teardown: `cleanup()` frees the PML4 page unconditionally (MP-7.3); the
  kslot-window tables are boot-shared and NOT freed individually; kernel-task
  user halves are empty so only the PML4 page is reclaimed.

### 7.1.1 Kernel-half region classification (MP-1.1 layout spec)

Which kernel-VA regions are per-task-private vs. shared-readonly vs. shared,
and how teardown treats each.  All values x86_64; other arches analogous.

| Region | PML4 idx | Entries | Class | Copied/created per task? | Freed at teardown? |
|---|---|---|---|---|---|
| Kernel text/data/bss (kernel map base, ~`0xFFFF8000...`) | 256..497 | PD/PT pages | **shared-readonly (text), shared (data/bss)** | copied **by value** (pointer-shared PD/PT) | **No** — boot-owned, refcounted by `PMM::is_user_page` false → skipped |
| HHDM direct map (`HHDM_OFFSET` + phys) | within kernel half | leaf/PD entries | **shared** (all tasks see all of RAM) | copied **by value** | **No** — boot-owned |
| kslot window (`CONFIG_KSTACK_WINDOW_BASE`, PML4 idx 498) | 498 | PDPT/PD/PT | **shared by reference** (window tables wired once at `init_kstack_window()`) | PD/PT entries inherited **by value**; no per-task tables | **No** — window tables boot-shared; per-slot guard pages remain |
| Deadline-monitor / misc kernel mappings | kernel half | — | shared | by value | No |
| User half (0xFFFF800000000000 below, PML4 idx 0..255) | 0..255 | — | **per-task-private** | zeroed for kernel tasks; ELF/deep-copy populated for user tasks | **Yes** — via `free_user_pages()` (user pages only) |
| Per-task kernel stack VA | inside kslot window | PT | private slot (guard page below) | slot allocated in shared window | slot/guard reclaimed by kslot allocator |

Classification rules (authoritative):
1. **Per-task-private:** only the user half (0..255) and the per-slot stack
   guard region.  Kernel tasks own an *empty* user half — exactly one PML4
   page is private and freed at teardown.
2. **Shared-readonly / shared:** every kernel-half entry (256..511) is copied
   **by value** from the boot kernel PML4 by `clone_kernel_pml4()`.  The
   underlying PD/PT pages are boot-owned; `free_user_pages()` never touches
   them because `PMM::is_user_page()` is false for all kernel PD/PT/leaf
   pages.  No task ever allocates a private kernel-half table.
3. **CR3 contract (MP-1.3):** because every task's `page_table_` is non-zero
   (a private PML4), `switch_to_task` always publishes
   `scheduler_load_cr3_from = next.page_table_`, and the ISR epilogue loads it
   for kernel AND user tasks alike.  The static `scheduler_kernel_cr3`
   fallback in `isr_stubs.asm` is hit only when no CR3 was published (a
   cleared/consumed switch), never for a normal kernel-task dispatch.
   Preserving HHDM + kernel text in every private table (rule 2) is what keeps
   kernel code running after the switch.
4. **Teardown (MP-1.4):** `cleanup()` → `VMM::free_user_pages(page_table_)`
   walks only user entries and frees only `PMM::is_user_page` pages (empty for
   a kernel task), then `PMM::free_page(page_table_)` reclaims the single
   private PML4 page.  The kslot window, HHDM and kernel text/data/bss pages
   are boot-shared and intentionally NOT freed individually.
