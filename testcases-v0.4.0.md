# Test Cases — v0.4.0 (Memory Protection)

## Branch: testbed only

*New tests to implement for the v0.4.0 Memory Protection milestone
(ROADMAP.md MP-1..MP-8).  Each test drives the real kernel path per the
driven-test cookbook (testcases-v0.3.10.md): create task prio ≥ 11 →
`Scheduler::add_task` → `reschedule()` → busy-wait `asm volatile("pause")`
until dispatched → assert on real state → `terminate_and_drain` →
`JARVIS_TEST_PASS()`.  No `set_current` impersonation, no direct
`task->state/priority/deadline_ticks` mutation, no fake `on_tick()`.
ResourceTracker snapshot baseline = zero delta.*

---

## MP-7 — `page_table_shared_` removal (deep-copy fork)

### testcases-v0.4.0-mp7.md → `test_pml4_clone.cpp` (rework + new)

- **fork_deep_copy_child_tables_independent**
  - **Testidea:** after `clone()`, the child's user page tables are fresh
    physical copies — the parent's PDPT/PD/PT pages are never aliased.
  - **Input:** parent task maps a user page at `va`; `clone()` a child; dump
    both PML4s.
  - **Expect:** for every present user PML4 entry, `child_pdpt_phys !=
    parent_pdpt_phys`; same for PD/PT at every level; data page is
    copy-on-write-free (same phys, distinct tables).
  - **Depends:** `VMM::clone_kernel_pml4`, deep-copy walk (MP-7.2).
- **fork_free_user_pages_child_deepcopy**
  - **Testidea:** `free_user_pages(child_pml4)` frees every copied table page
    AND the data page; parent mappings survive (phys entries unchanged).
  - **Input:** fork child with 1 user mapping; free child; re-walk parent.
  - **Expect:** child PML4/PD/PT/data all freed (PMM count returns to
    baseline); parent entry `(parent_virt[pml4_idx] & ~0xFFF) == pdpt` still.
  - **Depends:** MP-7.3 flag removal (free_user_pages no longer skips).
- **fork_page_table_shared_flag_absent**
  - **Testidea:** `TaskControlBlock::page_table_shared_` no longer exists
    (compile-time); a clone never shares table pages.
  - **Input:** compile + fork a child.
  - **Expect:** build succeeds with the field removed; child `page_table_` is
    a deep copy (MP-7.3).
  - **Depends:** MP-7.3.

---

## MP-1 — Private kernel-half page tables per kernel task

### `test_kernel_isolation.cpp` (NEW)

- **kernel_priv_pml4_clone_kernel_half_present**
  - **Testidea:** a kernel task's private PML4 has the kernel half
    (text/data/bss/HHDM) mapped so kernel code keeps running after CR3 switch.
  - **Input:** `VMM::clone_kernel_pml4()` for kernel half; inspect entries
    ≥ `PML4_USER_COUNT`.
  - **Expect:** kernel text/data PTEs present; HHDM direct-map present
    (REQ-MP-04); no user entries.
  - **Depends:** MP-1.2.
- **kernel_priv_cross_task_data_isolation**
  - **Testidea:** two kernel tasks have distinct private data pages — a write
    from task A is invisible to task B (separate PML4s).
  - **Input:** kernel task A writes `0xAA` to its private page; kernel task B
    (dispatched, own PML4) reads the same VA.
  - **Expect:** B faults or reads a different page (private frames differ);
    no corruption of A.
  - **Depends:** MP-1.3 (CR3 switch on kernel-task dispatch).
- **kernel_priv_cr3_switch_on_dispatch**
  - **Testidea:** dispatching a kernel task loads its private kernel PML4
    phys into CR3; returning to the idle/harness restores the original.
  - **Input:** dispatch a kernel task; read CR3 inside its lambda; read CR3
    in the harness.
  - **Expect:** CR3 differs while the kernel task runs; equals the system
    kernel CR3 when the harness runs; `scheduler_load_cr3_from` drives it.
  - **Depends:** MP-1.3.
- **kernel_priv_teardown_frees_pml4_stack**
  - **Testidea:** `cleanup()` frees the private kernel PML4 + its kernel-stack
    PT/PDPT/PD (mirror of `free_user_pages` + `free_stack_pdpt`).
  - **Input:** create + terminate + drain a kernel task.
  - **Expect:** zero PMM/MemPool delta; no leaked table pages.
  - **Depends:** MP-1.4.

---

## MP-6 — Kernel stack guard page

### `test_stack_alloc.cpp` (rework stubs + new)

- **kstack_guard_base_unmapped**
  - **Testidea:** the kslot base page (below the kernel stack) is not-present
    — an overflow #PFs instead of corrupting the next slot.
  - **Input:** allocate a kslot stack (`alloc_kslot`); walk the PT for the
    base VA.
  - **Expect:** base VA PTE not-present; `kernel_stack_top` PTE present.
  - **Depends:** MP-6.1.
- **kstack_overflow_invokes_hook**
  - **Testidea:** a kernel task overrunning `kernel_stack_top` reaches
    `CONFIG_STACK_OVERFLOW_HOOK` (panic with id + RIP) rather than silent
    corruption.
  - **Input:** kernel task lambda writes past `kernel_stack_top`; guard page
    #PF.
  - **Expect:** hook fires (or deterministic panic) with task id + RIP;
    harness/system survives the fault handling.
  - **Depends:** MP-6.2.
- **kstack_slot_snapshot_restore_safe**
  - **Testidea:** the kslot page-table pool survives `snapshot_restore` — a
    test that allocates a kslot slot then restores yields no stale PT.
  - **Input:** test A allocs kslot; snapshot_restore runs; test B allocs
    kslot again.
  - **Expect:** no stale/dangling PT; ResourceTracker zero delta.
  - **Depends:** MP-6.3.

---

## MP-2 — MMU red-zone guard pages between segments

### `test_memory_safety.cpp` (new + rework)

- **user_red_zone_stack_overflow_pf**
  - **Testidea:** a user task writing past its stack red-zone #PFs (unmapped
    guard page), not corrupting the heap/text neighbour.
  - **Input:** user task writes below `STACK_VADDR`-guard or past
    `program_break`; real #PF dispatch.
  - **Expect:** page-fault handler terminates the task cleanly; kernel alive;
    neighbour segment bytes unchanged.
  - **Depends:** MP-2.2 (red-zone gaps installed by ELF loader/brk).
- **user_red_zone_heap_overflow_pf**
  - **Testidea:** `brk` growth reserves an unmapped gap above the heap.
  - **Input:** task grows heap to the cap, then writes past it.
  - **Expect:** #PF (not corruption); task terminated.
  - **Depends:** MP-2.2.
- **kernel_red_zone_between_stack_data**
  - **Testidea:** MP-1 private kernel PML4 leaves an unmapped page between the
    kernel stack frame and adjacent kernel data.
  - **Input:** kernel task writes past `kernel_stack_top` toward kernel data.
  - **Expect:** #PF before touching the neighbour mapping.
  - **Depends:** MP-2.3.

---

## MP-3 — Software sentinel canaries

### `test_memory_safety.cpp` (new)

- **canary_installed_at_segment_boundaries**
  - **Testidea:** after ELF load / heap grow / stack setup, the canary magic
    sits in the slots before/after each segment.
  - **Input:** inspect the TCB canary fields + the segment guard slots.
  - **Expect:** `canary_before`/`canary_after` hold the known magic for
    text/data/heap/stack.
  - **Depends:** MP-3.2.
- **canary_tamper_detected_on_syscall**
  - **Testidea:** corrupting a canary is detected on syscall entry (panic or
    detection callback).
  - **Input:** write `0xDD` over `canary_after[stack]`; invoke a trivial
    syscall.
  - **Expect:** detection fires (gated callback, not a silent pass).
  - **Depends:** MP-3.3.
- **canary_intact_after_normal_dispatch**
  - **Testidea:** normal dispatch + syscall leaves canaries intact (no false
    positive).
  - **Input:** task runs, syscalls, exits.
  - **Expect:** all canaries match; no detection triggered.
  - **Depends:** MP-3.3.

---

## MP-4 — SMAP/SMEP (x86_64) — recommended-not-mandatory

### `test_cross_arch.cpp` (new, gated on CONFIG_SMAP/CONFIG_SMEP)

- **smep_user_exec_kernel_va_pf**
  - **Testidea:** a user task executing a kernel VA triggers SMEP #PF.
  - **Input:** user task `jmp` to a kernel-text VA; real dispatch.
  - **Expect:** #PF (SMEP), task terminated; kernel intact.
  - **Depends:** MP-4.1 (CR4.SMEP).
- **smap_kernel_deref_user_va_without_ac_pf**
  - **Testidea:** kernel deref of a user VA without `stac` triggers SMAP #PF.
  - **Input:** kernel path reads a user pointer with AC=0.
  - **Expect:** #PF; with `stac`/`clac` wrapper the read succeeds.
  - **Depends:** MP-4.2/4.3.
- **smap_stac_clac_roundtrip_ok**
  - **Testidea:** the AC-wrapped accessor reads/writes user memory correctly.
  - **Input:** `copy_from_user`-style accessor on a real user buffer.
  - **Expect:** data round-trips; AC restored after `clac`.
  - **Depends:** MP-4.3.

---

## MP-5 — Verification suite (cross-topic)

### `test_memory_isolation.cpp` (NEW)

- **cross_task_page_fault_isolated**
  - **Testidea:** task A maps a private page; task B derefs A's VA → #PF;
    B terminated, A + kernel alive.
  - **Input:** two dispatched kernel tasks, one mapping one not.
  - **Expect:** #PF recorded for B only; A's data intact; no system hang.
  - **Depends:** MP-1.5.
- **hhdm_kernel_reads_user_page**
  - **Testidea:** kernel reads a user page via the direct map (REQ-MP-04
    negative stays green).
  - **Input:** user task allocs page; kernel reads `HHDM_OFFSET + phys`.
  - **Expect:** read succeeds; value matches.
  - **Depends:** HHDM preserved in MP-1.2.
- **guard_page_fault_not_kernel_fatal**
  - **Testidea:** a user guard-page #PF kills only the faulting task.
  - **Input:** task overflows its red-zone; assert task state
    TERMINATED/REAPED and idle/shell continue.
  - **Expect:** harness/shell responsive after the fault (uptime works).
  - **Depends:** MP-2.2 + fault handler.

---

## MP-8 — Reworked existing tests (see ROADMAP MP-8)

- `test_pml4_clone.cpp` — rework `pml4_free_user_pages_shared_safe` to
  deep-copy semantics; keep `pml4_fork_user_entries_match` /
  `pml4_fork_no_child_corrupt_parent`.
- `test_page_tables.cpp` — replace 9 stubs with real allocate/install/walk/
  free round-trips on a cloned PML4.
- `test_stack_alloc.cpp` — replace 8 stubs with the MP-6 guard-page tests
  above.
- `test_process.cpp` — replace 12 stubs with real fork/clone/exit assertions
  under the deep-copy + private-kernel model.
- `test_memory.cpp` / `test_memory_safety.cpp` — fix pre-MP layout assertions
  (PT-page counts, free_user_pages skip semantics); keep the reserved-page
  invariant.

---

## Registration & count bookkeeping

- New classes: `kernel_isolation` (MP-1), `memory_isolation` (MP-5).
  New tests in existing classes: `memory_safety` (MP-2/MP-3),
  `cross_arch` (MP-4, gated), `pml4_clone`/`page_tables`/`stack_alloc`/
  `process` (MP-7/MP-8 rework).
- Update `test_expected_counts.hpp` for every touched class + the `all` row;
  run `dump_class_counts` to confirm no `[TCOUNT] MISMATCH`.
- Register new tests `JARVIS_REGISTER_TEST` (TF_KERNEL) unless
  MP-4 HW tests → gate behind `#if CONFIG_SMAP/CONFIG_SMEP`.
- Run `graphify update .` after code changes.

## Verification gates (per group, in order)

1. MP-7: `pml4_clone`, `process`, `memory` → 0 failures.
2. MP-1: `scheduler`, `process`, `kernel_isolation`, `pml4_clone` → 0.
3. MP-6: `stack_alloc`, `stack_profiler`, `scheduler` → 0.
4. MP-2/MP-3: `memory_safety`, `process`, `pmm` → 0.
5. MP-4: `cross_arch`, `memory_safety` → 0 (gated).
6. MP-5: `memory_isolation`, `memory_safety`, `cross_arch`, `pmm` → 0.
7. MP-8 rework: `pml4_clone`, `process`, `page_tables`, `stack_alloc`,
   `memory` → 0.
8. `make execute-test x86_64 debug selftest` → 132/132.
9. `make build` (check-style Errors: 0).
10. `make execute-test x86_64 debug all` → 835/835.
11. `make execute-test x86_64 release all` → 84/84 (trace OFF).
12. `test-history.txt` row after every class run.
