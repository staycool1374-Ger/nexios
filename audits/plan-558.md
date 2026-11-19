# PLAN: #558 / F5 — N4 VSPACE_MAP: map PageTable objects into a VSpace

```
Base: main @ b251b7b8 ("#557 F4: TCB_CONFIGURE/SUSPEND/RESUME (N1-N3), cap-gated task control",
      refs #557) — verified this session via the GitHub commits API on main (pushed tip).
      Local git log was not runnable in this session (no shell); the base is the pushed tip and
      the developer must re-verify with `git log -1 --format=%H` before the first edit and abort
      on any drift (predecessors F1 #554, F2 #555, F4 #557 are all in that history).
Lane: dev / main. Production kernel code only. src/kernel/test/** is READ-ONLY (test sanctity);
      new tests go to testbed via QE (TEST STRATEGY). Nothing is committed by the planner.
Cross-arch: ONE shared mechanism + the existing per-arch page-table codecs. No new per-arch
      backend and no new per-arch constant is required (see INV-VS-10); the per-arch difference
      (4-level x86_64/aarch64, 3-level riscv64) is read from the existing
      `VMM::vmm_arch_walk_levels()` (x86_64 vmm_x86_64.cpp:129 = 4, aarch64 vmm_aarch64.cpp:158
      = 4, riscv64 vmm_riscv64.cpp:139 = 3).
Normative reads: docs/specs/microkernel-transition.md §3.1 (107-117), §3.2 (119-193),
      §3.3 N4 row (202), §3.4 counts (212-220), §4.1-4.3 (230-263), §7.2 (426-439),
      §8.2 item 1 (495-522), §13.2-§13.3 (796-827), §14 M11 row (856), §15 F5 row (875),
      §16 Q3 (899) + Q9 (905); docs/specs/cspace.md §2.8.2 object table (408, 425-426);
      audits/plan-582-transition-compat.md (all 34 lines).
Graphify/vault retrieval (audit check-7 artifacts, all run this session):
  1. graphify query_graph "VSpace PageTable capability mapping address space" -> 94 nodes;
     surfaced the three per-arch ArchPageTable impls with their map_page/unmap_page/get_table/
     attr_from_flags codecs, the TLB family (arch_page_table_tlb_flush[_all],
     tlbi_vae1/aside1/alle1, tlb_purge_context), and audits/plan-554.md.
     DISPOSITION: USED — it named the exact codec seams this plan reuses (table_link/table_child
     live in the per-arch TUs) and confirmed no VSpace/PageTable consumer exists yet.
  2. graphify shortest_path "VSpace" -> "map_page_in_pml4": NO PATH.
     DISPOSITION: USED as staleness evidence — the graph has no VSpace/PageTable code node
     because F1 added vocabulary only; every anchor below was re-opened in the tree.
  3. graphify get_neighbors on the ArchPageTable cluster (aarch64 page_table_impl.hpp):
     calls {map_page, unmap_page, get_table, walk_l0..l3, split_l1_block, split_l2_block,
     attr_from_flags}, contains {arch_page_table_tlb_flush*, tlb_purge_context}.
     DISPOSITION: USED — fixes the arch boundary as "codec hooks only", i.e. no new per-arch
     file for F5.
  4. obsidian obsidian_search_vault "VSpace" (vault "jarvis") -> 17 hits: audits/plan-554.md,
     audits/design-review-554-r1.md:58 ("per-type ops deferred ... VSpace/PageTable->#558 F5"),
     audits/plan-555.md, audits/design-review-557-r1.md:23 ("No F5/#50 smuggling (carved_object
     doc-only, no VSPACE_MAP)"), specs/cspace.md:411/428, specs/microkernel-transition.md:205/
     240/642.
     DISPOSITION: USED — design-review-554-r1 is the F5 mandate (per-type ops are F5's duty) and
     design-review-557-r1 confirms F4 deliberately left VSPACE_MAP out, so nothing to rebase.
  5. obsidian search "PageTable" and "M11 object model" -> same audit/spec family, no additional
     note. DISPOSITION: IGNORED beyond the above (no vault note adds a constraint).
  6. obsidian search "VSPACE_MAP PageTable capability" -> 0 hits.
     DISPOSITION: recorded as a gap (no vault note is keyed on the syscall name), not a skip.
Staleness note: graph lags the tree (F1/F2/F4 are three commits old); files were treated as
  truth for every line number cited, and every anchor was re-opened at this base.
```

## AFFECTED FILES

- `src/kernel/cap/vspace_errors.hpp` (NEW) — X-macro `errors::VspaceError` (13 codes: OK + 12
  reachable), `error_string<VspaceError>`, `static_assert(VSPACE_ERR_OK == 0)`
  (pattern: cap_errors.hpp:32-74, tcb_control_errors.hpp as the newest precedent).
- `src/kernel/cap/vspace_map.hpp` (NEW) — `class VSpaceMap`: the bounded mapping registry +
  tree-link policy for VSpace/PageTable objects; `kMaxVspaceMappings` compile-time bound.
- `src/kernel/cap/vspace_map.cpp` (NEW) — the single mutation site: link a carved PageTable
  page at a path under a carved VSpace root, unlink/zero on teardown, bounded fixpoint drop of
  dependent slots, conservative TLB flush.
- `src/kernel/cap/carved_object.cpp` — `dispose()` (:71-77) and `revoke()` (:79-83) call
  `VSpaceMap::invalidate_table/invalidate_vspace` for the VSpace/PageTable kinds **before**
  the payload pages go back to the PMM; adds the per-consumer size assertion F1's review
  required (design-review-554-r1 S3:96-99).
- `src/kernel/cap/carved_object.hpp` — class doc (:36-39, :76-79) stops saying "no mapping
  invalidation at F1 … F5 must add it" and states the hooks it now has.
- `src/kernel/cap/cap_types.hpp` — doc comment only (:50-51: "wired by F5 #558" becomes
  "wired at F5 as VSPACE_MAP (syscall 102)"). No value, no enum, no ABI change.
- `src/kernel/cap/cap.hpp` — `remove()` doc (:194-201) gains the F5 sentence: deleting a
  mapped VSpace/PageTable cap destroys its link (destructive delete, INV-VS-8); declaration
  set unchanged.
- `src/kernel/memory/vmm.hpp` + `vmm.cpp` — five arch-seam wrappers over existing private
  codecs (`page_table_levels`, `current_root_phys`, `table_entry_is_table`,
  `table_entry_child`, `table_link_word`). No walk change, no new allocation path, no
  behavior change to any existing caller (each wrapper is one tail call — bit-identical by
  construction, LEARNINGS #363).
- `src/kernel/syscall/syscall_handlers_vspace.cpp` (NEW) — `Syscall::sys_vspace_map`
  (resolve → validate → authorize → mechanism → typed return), F4's
  syscall_handlers_tcb.cpp:36-138 layout.
- `src/kernel/syscall/syscall.hpp` — `VSPACE_MAP = 102` (:127-132 block), `MAX_SYSCALL = 103`,
  handler declaration, syscall-table row (:572-576 block), T-1 justification comment in-unit.
- `src/kernel/syscall/syscall_errors.hpp` — new `VSPACE_*` rows in a 1100+ block (:123-136 is
  the 1000+ precedent) + `from_vspace_error(uint64_t)` (:192-199 pattern).
- `src/libc/syscall.h` — `#define SYS_VSPACE_MAP 102` (next to :308-315), inline
  `sys_vspace_map()`, `SYS_MAX 103` + the :379-387 comment.
- `docs/specs/microkernel-transition.md` — §2 Syscalls row (:103), §3.2 new T-1 row 102,
  §3.3 N4 → resolved, §3.4 counts (:212-220), §4 a VSPACE_MAP bullet, §8.2 item 1 (:502),
  §14 M11 row (:856), §15 F5 row (:875), Appendix A record row.
- `docs/specs/cspace.md` — §2.8.2 fixed-size table rows (:425-426) + the retype-targets
  paragraph gain the VSPACE_MAP wiring sentence.
- NOT touched: `src/kernel/test/**` and `src/kernel/test/test_expected_counts.hpp` (QE),
  `src/kernel/nexios_config.h` (no new key — bounds live in cap/ per Q3/F9), `src/meson.build`
  (new .cpp files are auto-discovered by the `find` source glob), any PMM/MemPool code
  (F5 adds no allocation — see INV-VS-4).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-VS-1 single parent per table.** A carved PageTable page is linked under at most one
  parent slot in at most one VSpace, at any instant. Enforced at link time by a bounded
  `kMaxVspaceMappings` scan of the registry (phys identity), which also makes the tree acyclic
  by construction (a cycle needs one table to have two parents).
- **INV-VS-2 one owner per page, no allocation on the map path.** VSPACE_MAP writes exactly one
  8-byte table-link word in an already-existing parent page and allocates nothing: no PMM call,
  no MemPool call, no memset of a foreign page on the success path. PMM ownership of the child
  page stays with its `CarvedObjectCap` (F1), and of the root page with the VSpace cap. This is
  the whole point of N4: a pre-linked hierarchy makes the later leaf walk allocate-free
  (vmm_map.cpp:118-150 reuses an existing table entry via `get_table(..., true)`), which is
  INV-MK-1 / F21's requirement.
- **INV-VS-3 construction-only spaces.** No code in F5 activates a carved VSpace (nothing passes
  `vspace.phys` to `write_cr3`/`write_ttbr0`/SATP). Consequence: no CPU can hold a cached
  translation of a VA under a space being built, which is what makes the install path
  flush-free (INV-VS-5). Guarded defensively by `VSPACE_ERR_ACTIVE_SPACE` when the target root
  equals the *local* active root (`VMM::current_root_phys()`). M14 (proc-mgr) inherits the
  obligation to add a full activation guard before it ever activates a carved space.
- **INV-VS-4 removal is total and order-independent.** Both teardown entries
  (`invalidate_table`, `invalidate_vspace`) do, under the registry lock and in this order:
  (1) unlink the entry from its parent, (2) drop every registry slot that can no longer be
  unlinked because its parent chain is leaving (bounded fixpoint, INV-VS-7), (3) **zero the
  departing object's own page** (one bounded 4 KiB `__builtin_memset`). Step (3) is the
  authoritative kill: after it, no stale link inside that page can ever reach a freed page,
  whatever order F2's revoke cascade destroys the caps in. The page zeroing happens before
  `PMM::free_page` in `dispose()` — reversing that order hands a live parent a page the
  allocator may re-issue (S1, see risk 1).
- **INV-VS-5 flush discipline (per arch, one shared rule).** Any *removal* of a table link ends
  with a conservative `VMM::tlb_shootdown(0 /*va*/, 0 /*ctx*/, /*all=*/true)` executed after
  the registry lock is released: x86_64 `tlb_flush_all` (INVLPG-class coverage of
  paging-structure caches) + the weak `smp_tlb_shootdown_hook` (vmm.cpp:569-586), aarch64
  `arch_page_table_tlb_flush_all` (TLBI VMALLE1-class), riscv64 `sfence.vma` full. No
  narrowing to (ASID, VA) at F5 — no ASID/PCID is assigned to a carved space yet, and a narrow
  flush here would be an unproven optimisation on a security boundary. *Install* takes no flush:
  the parent entry was either zero or absent (checked) and the child page is zeroed by
  `CarvedObjectCap::create` (F1), so no VA under the new subtree ever had a valid translation
  that a CPU could have cached; INV-VS-3 removes the only other source (a previously-active
  space), and every removal already flushes.
- **INV-VS-6 intact-chain precondition for unlink.** Every live registry slot's parent chain is
  a sequence of live pages: a slot whose parent is going away is dropped in the same call that
  removes the parent (INV-VS-7), so `invalidate_table` never walks through a freed page.
- **INV-VS-7 bounded work everywhere.** Walk depth ≤ `page_table_levels() - 1` ≤ 3; registry
  scan ≤ `kMaxVspaceMappings`; the dependent-slot fixpoint is at most `kMaxVspaceMappings`
  passes over `kMaxVspaceMappings` slots (all loops have a compile-time bound, no
  `while(true)`); page zeroing is one 4 KiB store loop. No blocking wait, no reschedule, no
  unbounded scan.
- **INV-VS-8 mapping lifetime is bound to the PageTable cap.** There is **no** registry pin on
  either cap (deliberate divergence from FrameUserMap, which pins because its VMM unmap path
  dereferences the cap). Two reasons, both load-bearing: (a) the registry stores **physical
  addresses, never object pointers**, so no record can become a dangling dereference; (b) a pin
  would keep the page alive after the capability is deleted, with no name to unmap it — a
  permanent leak, since F5 adds no unmap syscall. Hence deletion is destructive
  (seL4-style): CAP_DELETE on a mapped PageTable/VSpace cap destroys the link and frees the
  page in the same last-release transition. This is the only option that leaves no stranded
  object, and it keeps INV-DERIV-5 intact (delete still never `revoke()`s anything).
- **INV-VS-9 lock discipline.** One TU-local leaf spinlock (`s_lock_`, `sync::SpinLock`,
  FrameUserMap shape) serialises both the registry and the tree word. Order:
  `CNode slot lock -> s_lock_` (the same take class as `cap::remove -> dispose ->
  FrameUserMap::invalidate_cap`); `s_lock_` never takes another lock, never calls
  VMM/PMM/MemPool/Logger, never reschedules, and is not held across the TLB flush (mutation and
  fixpoint under the lock, flush after the unlock). It is never taken with
  `scheduler_lock_` held, and never from an ISR path (the syscall and cap-teardown callers are
  task context, audits/plan-582-transition-compat.md rows 1-9 untouched).
- **INV-VS-10 arch neutrality is read, not asserted.** Depth comes from
  `VMM::vmm_arch_walk_levels()`; the user/kernel half boundary from
  `arch::PML4_USER_COUNT` (lib/constants.hpp:63-64, 256 on all three arches); entry count from
  `arch::ArchPageTable::ENTRIES` (512 on all three); descriptor encode/decode from the
  existing per-arch `vmm_arch_table_link/table_child/is_table` — so x86_64 raw-phys links,
  aarch64 `PAGE_PRESENT|PAGE_TABLE`, and riscv64 `sv39_phys_pte(ppn)|PAGE_PRESENT` all come
  from the one codec that the leaf walk already uses (bit-identity, no second encoder).
- **INV-VS-11 typed-error totality.** Every failure path writes exactly one `VspaceError`;
  the syscall face is `0` or `-(1100 + code)` (never a raw `-1`, syscall_errors.hpp:145-149
  discipline). `SYS_ERR_OK` on success. `arg` bits outside the declared path are rejected,
  never masked (LEARNINGS #337 truncation trap).

## STEP-BY-STEP CHANGES

1. **`cap/vspace_errors.hpp` (new).** `#pragma once`, `<types.hpp>`, `<assert.hpp>`,
   `namespace kernel::errors`, X-macro `VSPACE_ERROR_CODES` with
   `X(OK,0,...)`, `INVALID_ARGS 1`, `BAD_HANDLE 2`, `BAD_OBJECT 3` (handle resolves but is
   not a `CarvedObjectCap` of the expected kind/tag), `OWNERSHIP_MISMATCH 4` (PMM ownership
   differs between VSpace and PageTable), `BAD_LEVEL 5` (path_len 0 or > levels-1),
   `KERNEL_HALF 6` (root component ≥ `arch::PML4_USER_COUNT`), `SLOT_OCCUPIED 7`,
   `ALIASED 8`, `NOT_TABLE 9` (path component holds a leaf), `UNMAPPED 10` (path component
   absent while descending), `REGISTRY_FULL 11`, `ACTIVE_SPACE 12`.
   `enum VspaceError : uint64_t`, `error_string<VspaceError>`,
   `static_assert(VSPACE_ERR_OK == 0)`. Each code is reachable from exactly one branch of
   step 6/7 — no dead enum (CODING_STYLE §5.3).
2. **`memory/vmm.hpp` / `vmm.cpp` — five arch-seam wrappers (no logic).** Declare in the
   public section next to `current_pml4()` (vmm.hpp:134) and define in vmm.cpp beside
   `tlb_shootdown` (:579-586):
   `static unsigned page_table_levels() noexcept { return vmm_arch_walk_levels(); }`;
   `static uint64_t current_root_phys() noexcept { return vmm_arch_root_phys(current_pml4()); }`
   (x86 strips PCID via vmm_x86_64.cpp:49-53, aarch64 masks ASID via vmm_aarch64.cpp:49-56);
   `static bool table_entry_is_table(uint64_t entry) noexcept`;
   `static uint64_t table_entry_child(uint64_t entry) noexcept`;
   `static uint64_t table_link_word(uint64_t child_phys) noexcept`.
   Each body is a single `return vmm_arch_…();` — no branch, no state, no new error path.
   Doc-comment each with "F5 #558 seam; only the per-arch codec may decode/encode a table
   descriptor (LEARNINGS #363)". Do **not** add a VMM-side VSPACE_MAP walk (a VMM-returning
   variant was rejected: it would force a second error enum plus a translation table).
3. **`cap/vspace_map.hpp` (new).** `#pragma once`, `<types.hpp>`, `<kernel/cap/cap_types.hpp>`,
   `<kernel/cap/object_sizing.hpp>`, `<kernel/sync/spinlock.hpp>`.
   `constexpr size_t kMaxVspaceMappings = 32;` with a comment stating the provenance:
   "placeholder sizing owned by F9 #562 (page-table bounds); 32 slots cover 10+ distinct 4-level
   subtrees. Lives here (cap/) per Q3 — do not add a nexios_config.h key; if F9 re-sizes page
   tables, this constant moves with it, it is not duplicated.".
   `class VSpaceMap` with: `[[nodiscard]] static errors::VspaceError link(uint64_t
   vspace_phys, uint64_t table_phys, uint64_t path, uint8_t path_len) noexcept`;
   `static void invalidate_table(uint64_t table_phys) noexcept`;
   `static void invalidate_vspace(uint64_t vspace_phys) noexcept`;
   `static void snapshot_reset() noexcept` (test isolation);
   `static size_t live_count() noexcept` and `static bool is_linked(uint64_t table_phys) noexcept`
   (test accessors). Private: `struct Slot { uint64_t vspace_phys; uint64_t table_phys;
   uint64_t parent_phys; uint64_t path; uint8_t path_len; bool occupied; }` (phys identity only,
   INV-VS-8), `static Slot s_slots_[kMaxVspaceMappings]; static sync::SpinLock s_lock_;`
   and private helpers `component(path, k)`, `walk_parent(root_phys, path, path_len,
   uint64_t &parent_out)`, `drop_dependents(uint64_t dying_phys)`, `zero_page(uint64_t phys)`.
   Class doc carries INV-VS-1/4/5/8/9 in one paragraph each.
4. **`cap/vspace_map.cpp` (new).** Includes `vspace_map.hpp`, `vspace_errors.hpp`,
   `carved_object.hpp`, `<kernel/memory/vmm.hpp>`, `<kernel/memory/pmm.hpp>`,
   `<kernel/sync/spinlock_guard.hpp>`, `<constants.hpp>` (`arch::HHDM_OFFSET`,
   `arch::PAGE_SIZE`, `arch::PML4_USER_COUNT`).
   (a) Compile-time fail-closed (the per-consumer check design-review-554-r1 S3:96-99 demands;
   here as equality, because a VSpace/PageTable object **is** one arch page-table page and has
   no C++ struct):
   `static_assert(arch::ArchPageTable::ENTRIES * sizeof(uint64_t) == kObjectBytesVSpace)` and
   the same for `kObjectBytesPageTable`; `static_assert(kMaxVspaceMappings > 0)`.
   (b) `component(path, k)` = `(path >> (9 * k)) & 0x1FF`; callers guarantee `k < 4` and a
   9-bit field ≤ 511 = `ENTRIES - 1`, so every derived index is in range by construction.
   (c) `link()` order — **argument validation before any shift** (a `path_len` of 255 would
   make `9 * path_len` undefined/over-wide; LEARNINGS #337 class):
   `path_len == 0 || path_len > VMM::page_table_levels() - 1` -> BAD_LEVEL (compare first,
   never shifted); `(path >> (9 * static_cast<unsigned>(path_len))) != 0` -> INVALID_ARGS;
   `component(path, 0) >= arch::PML4_USER_COUNT` -> KERNEL_HALF;
   `vspace_phys == table_phys` -> ALIASED; `vspace_phys == VMM::current_root_phys()` ->
   ACTIVE_SPACE; `(vspace_phys | table_phys) & (arch::PAGE_SIZE - 1)` -> INVALID_ARGS;
   then, under `SpinLockGuard`: walk components `0 .. path_len-2` from the HHDM base of
   `vspace_phys` (`!entry` -> UNMAPPED, `!VMM::table_entry_is_table(entry)` -> NOT_TABLE,
   descend via `VMM::table_entry_child`), check the destination slot is zero
   (-> SLOT_OCCUPIED), scan the registry for `table_phys` (-> ALIASED), claim a free slot
   (-> REGISTRY_FULL), store `VMM::table_link_word(table_phys)` in the destination — the one
   mutation — and record the slot with `parent_phys` = the page the store landed in.
   Return OK. No TLB flush (INV-VS-5).
   (d) `invalidate_table(table_phys)`: under the lock — find the slot with
   `table_phys`; if found, `walk_parent` its path and clear the entry **only after**
   confirming the entry equals `VMM::table_link_word(table_phys)` (a re-used path must not be
   clobbered); drop the slot; `drop_dependents(table_phys)` (fixpoint: drop every slot whose
   `parent_phys` is in the dropped set, at most `kMaxVspaceMappings` passes);
   `zero_page(table_phys)`. Unlock, then `VMM::tlb_shootdown(0, 0, true)`.
   Idempotent (a second call finds no slot and only re-zeroes a page that is still owned by the
   caller). Comment: "M14: once a VSpace can be activated, the flush must move *before* the
   zero — no space is active at F5 (INV-VS-3)".
   (e) `invalidate_vspace(vspace_phys)`: under the lock drop every slot with
   `vspace_phys`, then `zero_page(vspace_phys)` (kills every link inside the root in one
   bounded store, INV-VS-4); unlock; `tlb_shootdown(0, 0, true)`.
   (f) `snapshot_reset()`: clear all slots (no page touching — the pages belong to fixtures
   whose PMM/MemPool state is rewound by the snapshot); no pin accounting exists to unwind
   (INV-VS-8), which is why this stays 10 lines.
   (g) `live_count`/`is_linked` under the lock.
5. **`cap/carved_object.cpp` — hook the teardown (the exact place F1 left marked).**
   In `revoke()` (:79-83) and `dispose()` (:71-77), before any `PMM::free_page`:
   `if (kind == CapType::PageTable) VSpaceMap::invalidate_table(phys); else if (kind ==
   CapType::VSpace) VSpaceMap::invalidate_vspace(phys);` then `KernelObject::revoke()` /
   the existing dispose body unchanged. Both hooks are idempotent, so revoke-then-dispose is
   safe. Add the two size `static_assert`s of step 4(a) here **or** in step 4 — one site
   only, chosen: step 4(b) TU (it is the VSpace/PageTable consumer). Update the class doc
   (:36-39) and the `revoke()` comment (which currently promises "F5 must add its
   invalidation here") to state what is now done.
6. **`syscall/syscall_handlers_vspace.cpp` (new) — `Syscall::sys_vspace_map`.**
   `arg0` = VSpace cap handle, `arg1` = PageTable cap handle, `arg2` = packed path
   (component k at bits [9k .. 9k+8], root first), `arg3` = path_len.
   Body: resolve the caller CSpace (`shm_current_cspace()` shape,
   syscall_handlers_shm.cpp:43-46); `lookup(cs, arg0, CapType::VSpace, CapRights::WRITE)`
   and `lookup(cs, arg1, CapType::PageTable, CapRights::READ)` (cap.hpp:176-182 — returns a
   pinned object, driver must `release()` both on every exit path, ScopeGuard per
   CODING_STYLE §7.1); reject a null result with `BAD_HANDLE`; reject a
   `static_cast<CarvedObjectCap*>` whose `tag != kCarvedObjectTag` or `kind` mismatched with
   `BAD_OBJECT` (F4 S1(a) stale-slot pattern, tcb_cap.hpp:45-50); reject
   `vs->is_user != pt->is_user` with OWNERSHIP_MISMATCH; then
   `VSpaceMap::link(vs->phys, pt->phys, arg2, static_cast<uint8_t>(arg3))`.
   Return `SYS_ERR_OK` or `-(1100 + code)` via `errors::from_vspace_error`; one
   `#if defined(CONFIG_DEBUG)` debugcon diagnostic on the failure path (diagnostic only, no
   control-flow divergence — CODING_STYLE §6). No other argument is accepted: `arg4`/regs
   pointer is unused (reject nothing there — it is the shared ABI slot).
7. **`syscall/syscall.hpp`.** After `TCB_RESUME = 101` (:131):
   `VSPACE_MAP = 102, ///< N4 map a PageTable object into a VSpace (F5, issue #558): arg0 =
   VSpace handle (WRITE), arg1 = PageTable handle (READ), arg2 = packed 9-bit path, arg3 =
   path length`, and `MAX_SYSCALL = 103` (:132). Declare
   `static uint64_t sys_vspace_map(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t *);`
   beside `sys_tcb_configure`, and add the table row `&Syscall::sys_vspace_map, // 102
   VSPACE_MAP (M11, F5 #558)` after :576. Above the enum, the in-unit T-1 justification block
   (F4 precedent, spec §3.1 / feeder #80) — text to reuse verbatim in the #558 comment:
   "N4 exists because INV-MK-1 cannot be met without it: proc-mgr (§8.8) and mem-mgr (§8.2) must
   build page-table hierarchies from carved PageTable caps, and a hierarchy assembled without an
   object-level link operation can only be built by the kernel allocating tables on the caller's
   behalf. VSPACE_MAP adds no authority beyond the two caps it consumes (WRITE on the VSpace,
   READ on the PageTable), allocates nothing, and is bounded by the arch depth (≤3 components),
   a 32-entry registry and one 8-byte store."
8. **`syscall/syscall_errors.hpp`.** New block `/* VSpace mapping errors (1100+, F5 #558) */`
   with `VSPACE_INVALID_ARGS 1101`, `VSPACE_BAD_HANDLE 1102`, `VSPACE_BAD_OBJECT 1103`,
   `VSPACE_OWNERSHIP_MISMATCH 1104`, `VSPACE_BAD_LEVEL 1105`, `VSPACE_KERNEL_HALF 1106`,
   `VSPACE_SLOT_OCCUPIED 1107`, `VSPACE_ALIASED 1108`, `VSPACE_NOT_TABLE 1109`,
   `VSPACE_UNMAPPED 1110`, `VSPACE_REGISTRY_FULL 1111`, `VSPACE_ACTIVE_SPACE 1112`;
   plus `inline SyscallError from_vspace_error(uint64_t e) { return
   static_cast<SyscallError>(1100 + e); }` next to `from_tcb_control_error` (:192-199).
9. **`src/libc/syscall.h`.** `#define SYS_VSPACE_MAP 102` after :315, an inline
   `static inline long sys_vspace_map(unsigned long vspace_handle, unsigned long
   table_handle, unsigned long path, unsigned long path_len)` next to `sys_tcb_suspend`
   (:371-377), and `SYS_MAX 103` with the :379-387 comment extended by one line
   ("102 is VSPACE_MAP (F5 #558)").
10. **`cap/cap_types.hpp` + `cap/cap.hpp` docs.** `:50-51` reworded to "wired by F5 #558 as
    VSPACE_MAP (syscall 102)"; `cap.hpp:194-201` `remove()` doc gains: "Deleting a mapped
    VSpace/PageTable cap destroys its table link on the last release (INV-VS-8, F5 #558) — a
    destructive delete, still never a `revoke()` (INV-DERIV-5)."
11. **Docs.** `docs/specs/microkernel-transition.md`: §2 row (:103) -> `MAX_SYSCALL = 103`;
    §3.2 append `| 102 | VSPACE_MAP | live (M11, F5 #558) | KEEP |`; §3.3 N4 row (:202) ->
    "N4 → 102 RESOLVED at F5 (#558)"; §3.4 (:212-220) -> KEEP 52 (adds 102), New N7–N9 = 3,
    certified total 55 unchanged, 0–102 = 103 numbers and `MAX_SYSCALL` = 103; §4 gains a
    short "VSPACE_MAP (N4)" paragraph under §4.3 stating INV-VS-1/5/8 (single parent,
    flush on removal, destructive delete); §8.2 item 1 (:502) "adds N4" -> "adds N4 (landed,
    102)"; §14 M11 row (:856) marks F5 landed; §15 F5 row (:875) records the syscall number;
    Appendix A gains the implementation record row. `docs/specs/cspace.md`: object table rows
    (:425-426) become "VSpace | 1 page | F5 #558 — address-space root, wired by VSPACE_MAP"
    and the same for PageTable; the retype-targets paragraph notes that PageTable/VSpace caps
    are the only kinds with an object-level map operation.
12. **Verification sweep (developer).** `tools/meson/build.sh` for x86_64, aarch64, riscv64
    (new TUs are auto-discovered, but the source-set change must be re-verified: `meson test -C
    build/meson-<arch>` for the host-side parity/policy tests); `make check-style`
    (Errors: 0); then TEST STRATEGY classes one at a time with a `test-history.txt` row each
    (measured TIME, never estimated); then `graphify update .`; then the SIL 3 audit on a
    `git diff main` patch (production change ⇒ audit mandatory), and the audit evidence +
    DECISION posted on #558.

## TEST STRATEGY

- **No test file is modified or added on main.** `src/kernel/test/**` and
  `test_expected_counts.hpp` are QE-owned; the F5 obligation is the sketch below, handed to
  testbed.
- **Existing classes to run, in this order** (fix-one-at-a-time; timeouts from
  `test-history.txt` × 1.5):
  1. `cap_core` — the carved-object/lifecycle TU is the direct owner of the changed
     `CarvedObjectCap::dispose/revoke` (test_registry.cpp:803).
  2. `cap_untyped` — retype of the VSpace/PageTable kinds must stay green (F1's
     `retype_target_types`/lifecycle rows are the regression oracle).
  3. `cap_syscall` — new dispatch number, handle encoding, policy word.
  4. `capability` — M11 gate class (aggregate, test_registry.cpp:1196-1215).
  5. `proc_elf` — M11 gate class (:1220+; ELF/address-space paths downstream of the VMM
     seam).
  6. `core_memory` — the five VMM seam wrappers are the only VMM-adjacent edit.
  7. `core_syscall` — MAX_SYSCALL 102→103 ripples through the surface/fuzz/fastpath tests.
  8. `selftest` — CI gate.
- **Cross-arch gate (mandatory):** `build.sh` on all three arches plus `cap_core`,
  `capability`, `proc_elf` on aarch64 and riscv64 (`capability`/`proc_elf` carry no
  `arch_scope` row in tools/test-policy.tsv, so the default `arch_scope|any` applies by
  absence — :22/:30, not an explicit per-class row). The release-version gate remains the 2×3
  matrix `bash scripts/run_all_classes.sh {x86_64,aarch64,riscv64} {debug,release}`.
- **New tests handed to QE (testbed, stub-first, `JARVIS_REGISTER_TEST`, one new fine class
  `cap_vspace` registered into the `capability` aggregate; `test_expected_counts.hpp` rows
  bumped for both the fine and the aggregate class; `test_isolate.cpp` gains
  `cap::VSpaceMap::snapshot_reset();` next to `FrameUserMap::snapshot_reset()` (:1275))**:
  1. `mk_vspace_map_links_page_table` — retype VSpace+PageTable from one Untyped; syscall 102
     with path component 0, len 1 returns `SYS_ERR_OK`; the root entry, read through
     `arch::HHDM_OFFSET + vspace_phys`, equals `VMM::table_link_word(pt_phys)`;
     `VSpaceMap::live_count() == 1`.
  2. `mk_vspace_map_second_level` — link at [i0], then [i0,i1] onto the first table; both
     succeed; a third component on riscv64 (len 3) is BAD_LEVEL, while x86_64/aarch64 accept
     len 3 and reject len 4 (assert the *per-arch* limit from `VMM::page_table_levels()`,
     never a literal).
  3. `mk_vspace_map_occupied_slot` — a second link at the same path with a *different*
     PageTable returns `VSPACE_SLOT_OCCUPIED`, the first entry is unchanged and
     `live_count()` is unchanged.
  4. `mk_vspace_map_aliased_table` — the same PageTable object at a second path returns
     `VSPACE_ALIASED` (INV-VS-1) and writes nothing.
  5. `mk_vspace_map_kernel_half_rejected` — component 0 = 256 and = 511 return
     `VSPACE_KERNEL_HALF`; the root page is bit-identical before/after (no silent install).
  6. `mk_vspace_map_path_bits_rejected` — `path` with a bit above `9*len` returns
     `VSPACE_INVALID_ARGS`; `path_len` 0 and `page_table_levels()` return
     `VSPACE_BAD_LEVEL`; a valid `path_len` with garbage high bits is never masked.
  7. `mk_vspace_map_delete_unlinks` — CAP_DELETE (98) on the PageTable handle destroys the
     link (root entry reads 0) and frees the registry slot (INV-VS-8); a second CAP_DELETE
     fails idempotently and the VSpace is otherwise intact.
  8. `mk_vspace_map_vspace_delete_zeroes_tree` — with a 2-level subtree, CAP_DELETE on the
     *VSpace* handle leaves the whole root page zeroed and `live_count() == 0`, and the
     orphaned PageTable caps are still deletable afterwards (no double free).
  9. `mk_vspace_map_revoke_cascade` — `cap::revoke_derivations` on the parent Untyped
     destroys VSpace + all PageTables; `live_count() == 0`; ResourceTracker delta 0
     (PMM, MemPool0, cap_objects) — this is the F2/F4.3 cascade proof.
  10. `mk_vspace_map_wrong_cap_type` — a Frame handle in arg0, a VSpace handle in arg1, and a
      VSpace slot minted without WRITE all fail (`BAD_HANDLE`/`POLICY_DENIED`); a real
      `Task`-typed object in arg1 gives `BAD_HANDLE` (type gate before the cast).
  11. `mk_vspace_map_ownership_mismatch` — a kernel-owned PageTable (carved from a
      `is_user=false` Untyped) into a user VSpace returns `VSPACE_OWNERSHIP_MISMATCH`.
  12. `sys_vspace_map_dispatch_end_to_end` — a real user task issues `int 0x80` 102 with a
      granted VSpace+PageTable pair and observes 0; the surface test sees
      `MAX_SYSCALL == 103` and VSPACE_MAP present.
  13. `mk_vspace_map_leaf_population_alloc_free` — after building root→PD→PT via 102, a
      `VMM::map_page_in_pml4_try(va, frame, true, false, vspace_phys)` succeeds with **zero**
      PMM delta (the pre-linked walk allocates nothing — the INV-MK-1 rationale). `va` comes
      from a HAL constant per arch, never a literal (LEARNINGS #387).
- **Tracker/oracle:** every listed run must show ResourceTracker delta 0. No new counter type
  is introduced (a registry slot is not a resource), so `test_isolate`'s buffer layout is
  untouched — only the `snapshot_reset()` call line is added.

## SIL 3 / SAFETY RISKS

- [S1] **Free-then-linked page (dangling table pointer)** — if `dispose()` frees the payload
  before the link is cleared/zeroed, a parent table points at a page the PMM may re-issue, and
  the next walk treats arbitrary memory as a page table. Mitigation: INV-VS-4 order is
  unconditional (invalidate → unlink → drop dependents → zero page → `PMM::free_page`), and QE
  tests 7-9 assert the resulting state; the F2 revoke-cascade order cannot break it because
  the zero is order-independent.
- [S1] **Kernel-half link (privilege escalation)** — a link installed at root index ≥
  `arch::PML4_USER_COUNT` would let a caller-shaped table share the kernel half
  (x86_64/aarch64 upper half, riscv64 above `USER_VA_LIMIT`). Mitigation: KERNEL_HALF check
  before the walk (step 4c) + QE test 5 asserting a bit-identical root page.
- [S1] **Aliased/double-parented table** — one page linked under two parents yields two
  unlink paths (double-unlink → clobbering a live entry) and a cyclic tree (unbounded walk).
  Mitigation: INV-VS-1 + registry phys scan + `vspace_phys != table_phys`; QE test 4.
- [S1] **Silent overwrite of a live link** — overwriting an occupied slot orphans a subtree
  whose registry record then unlinks a foreign path. Mitigation: SLOT_OCCUPIED, checked under
  the same lock that performs the store; QE test 3.
- [S1] **Argument truncation UB** — `9 * path_len` with an unvalidated `path_len` (the fuzz
  class feeds 0..MAX-1 with extreme args) is over-wide/undefined, and masking instead of
  rejecting a packed path with high bits set would install into a wrong slot. Mitigation:
  bounds check *before* any shift, then reject `path >> (9*len) != 0` (never mask);
  QE test 6; `test_syscall_fuzz` exercises 102 automatically.
- [S1] **Kernel pages becoming user page tables** — linking an `is_user=false` PageTable into a
  user VSpace exposes kernel RAM as an EL0-visible hierarchy (the FrameUserMap
  `!fc.is_user` refusal is the precedent, frame_map.cpp:84-85). Mitigation:
  OWNERSHIP_MISMATCH on `vs->is_user != pt->is_user`; QE test 11.
- [S2] **Stale TLB / paging-structure caches after a removal** — a retired table can stay
  cached (x86 paging-structure caches, aarch64 ASID-tagged entries, riscv ASID entries),
  leaving a translation live into a freed page. Mitigation: INV-VS-5 full `tlb_shootdown(0,0,
  true)` on every removal, executed after the lock; the install path is flush-free only
  because it is provably new (INV-VS-3) — both halves stated in code comments so M14 cannot
  narrow one without the other.
- [S2] **Registry staleness across snapshot restore** — a slot surviving the debug-suite
  snapshot could later match a re-issued page. Mitigation: slots store physical addresses and
  never dereference anything (INV-VS-8), `invalidate*` re-validates the parent entry word
  before clearing, and QE adds `snapshot_reset()` to test_isolate.cpp:1275; if that line is
  missing the failure is a failed F5 test, not corruption (phys mismatch).
- [S2] **Double-release / pin accounting** — deliberately avoided: no pins exist, so the
  #320/#366 over-release class cannot occur on this path. State the divergence from
  FrameUserMap explicitly in `vspace_map.hpp` so a later "consistency" edit does not add pins
  back without re-deriving the leak argument (INV-VS-8).
- [S2] **F21 (#573) / F9 (#562) coordination** — F5 adds no PMM entry point, so it needs no
  `CONFIG_STATIC_POOLS_ONLY` exemption and no new `nexios_config.h` key. The *retype* side
  (VSpace/PageTable carve) already allocates through PMM and becomes F21's concern; #562 owns
  the eventual page-table bound row. Mitigation: plan touches neither file; if either lands
  first, rebase and re-run the gate classes (ordering note, no duplicate work).
- [S3] **Closed-set accounting drift** — MAX_SYSCALL 103 without the T-1/§3.4 update leaves the
  spec's "every number has exactly one disposition" claim false. Mitigation: step 11 is part of
  the same commit; the SIL 3 justification (step 7 text) is posted on #558 before
  implementation (feeder #80).
- [S3] **Style/checker gates** — new headers need `#pragma once`, ≤80 columns, no
  single-character names (avoid `i`/`j` at TU scope; loop indices only in ≤5-line bodies),
  no magic numbers outside the sizing/error headers, `NOLINTBEGIN(bugprone-easily-swappable-
  parameters)` on `link(vspace_phys, table_phys, path, path_len)` (precedent
  carved_object.hpp:67-70), fully bounded loops. `make check-style` must report Errors: 0.
- [S3] **Spec doc-lie risk** — §4.1 still lists VSpace/PageTable as "required additions (F1)";
  after F5 they are live objects with a live syscall. Step 11 closes it in the same commit
  (precedent: F2 #555 and F4 #557 both edited the normative spec in-lane with owner sign-off
  recorded on the issue).

## OPEN QUESTIONS

- **Q-f5-1 (owner, non-blocking): destructive delete of a mapped PageTable (INV-VS-8).** Plan
  takes seL4-style destructive delete (CAP_DELETE destroys the link), because F5 adds no unmap
  syscall — the alternative (fail closed with "still mapped") would strand an object with no
  recovery path. If the owner prefers a future `VSPACE_UNMAP`, it is a separate T-1 entry and
  not a blocker: no number is burned and the registry already stores every field an unmap needs.
- **Q-f5-2 (owner, non-blocking): `kMaxVspaceMappings = 32` is a placeholder** pending F9
  (#562) page-table bound sizing; it lives in `cap/vspace_map.hpp` per Q3 (no
  `nexios_config.h` key). Implementation proceeds at 32; a later F9 row replaces the constant.
