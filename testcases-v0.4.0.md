# Test Cases — v0.4.0 (Memory Protection)

## Branch: testbed only

*Backlog — only MP-7's three spec-named tests remain. MP-1 (kernel_isolation
4/4), MP-2/3 (memory_safety 6/6), MP-4 (cross_arch SMAP/SMEP 3/3, gated),
MP-5 (memory_isolation 3/3), MP-6 (stack_alloc 3/3) and the MP-8 rework
(page_tables 9, process 17, stack_alloc 11, pml4_clone 7) are all implemented
and registered (verified 2026-08-15). The compile-time `page_table_shared_`
removal and `VMM::deep_copy_user_pages` machinery exist; only the exact
spec-named MP-7 functions are absent. Functional coverage of the same intent
already exists via `pml4_deep_copy_no_alias`, `pml4_free_user_pages_shared_safe`
(reworked to deep-copy), `pml4_fork_user_entries_match`,
`pml4_fork_no_child_corrupt_parent`.*

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

## Registration & count bookkeeping

- Update `test_expected_counts.hpp` for every touched class + the `all` row;
  run `dump_class_counts` to confirm no `[TCOUNT] MISMATCH`.
- Register new tests `JARVIS_REGISTER_TEST` (TF_KERNEL).

## Verification gates (per group, in order)

1. MP-7: `process` (contains `pml4_clone` tests), `memory` → 0 failures.
2. `make execute-test x86_64 debug selftest` → 132/132.
3. `make build` (check-style Errors: 0).
4. `make execute-test x86_64 debug all` → 883/883.
5. `make execute-test x86_64 release all` → 84/84 (trace OFF).
6. `test-history.txt` row after every class run.
