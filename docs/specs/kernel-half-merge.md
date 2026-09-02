# Kernel-Half Merge — Lazy Page-Table Synchronization

**Doc ID:** NEX-SPEC-2026-08-23-004
**Status:** DRAFT
**Milestone target:** v0.4.5 (post-fork hardening / exec fast path)
**Inspiration:** Cyjon `kernel/page.asm` `kernel_page_merge` (~line 595–700):
instead of copying kernel page-table entries into a new process PML4, walk
PML4→PML1 and map in **only the missing kernel entries**.
**Related:** `docs/specs/memory.md` (§ kernel-half copy-by-value table,
MP-7 fork deep-copy spec), `_archive/fork-pt-deep-copy.md`.

## 1. Current State (verified)

NexIOS fork performs a full recursive copy of the child address space,
including the kernel half: memory.md's mapping table marks HHDM direct-map
entries "copied **by value**" and boot-owned. That works, but:

- every fork pays O(kernel-half entries) work even though the kernel half
  never differs between tasks;
- two sources of truth exist — if the kernel grows a mapping after a task
  was forked, older PML4s don't have it (today mitigated only because all
  kernel mappings exist before the first user task);
- MP-7's deep-copy was the correct *correctness* milestone; this spec is the
  follow-up *efficiency* milestone.

## 2. The Merge Idea (adapted)

On fork (and execve image setup):

1. Allocate fresh PML4 for the child; copy ONLY the user half
   (entries 0..255) recursively — exactly what MP-7 already does for user
   data.
2. For the kernel half, run **merge**: walk parent PML4[256..511] top-down
   (PDPT→PD→PT levels); wherever the child lacks an entry, link/copy it from
   the parent. Entries that already exist are left untouched.
3. Result: the child's kernel half converges to the canonical kernel image
   without a full duplicate walk, and future-added mappings can be merged
   lazily on demand.

Difference vs. Cyjon: they merge to guarantee presence of kernel mappings
in arbitrary process tables (no isolation goal); we merge **within our
existing ownership rules** (memory.md: HHDM stays shared read-only concept,
boot-owned, never writable from user) — i.e., merge copies *page-table
structure*, never grants new access rights beyond the kernel-half policy
already in force.

## 3. Design Details

- Merge direction: always parent → child, single-threaded, executed before
  the first CR3 switch to the child (same window MP-7 uses ⇒ no locking
  needed; document as invariant).
- Idempotent: merging twice must be a no-op (entry-present check first).
  This makes it safe to also run merge opportunistically at execve if a
  stale PML4 is reused from a cache (future optimization).
- Failure mode: allocation failure mid-merge ⇒ destroy child PML4 entirely
  (existing fork cleanup path), return -ENOMEM to caller. Never return a
  partially merged table.
- Debug assertion: after merge, compare child kernel half against the boot
  template PML4 (`pml4_kernel_template`, snapshot taken at end of bring-up);
  mismatch ⇒ panic. This gives us a continuous structural audit for free.

## 4. Semantics Gained

| Property | Today (full copy) | With merge |
|---|---|---|
| Fork cost | user half + kernel half | user half only |
| New kernel mapping visible to pre-existing tasks | no | on next merge point |
| Structural audit vs boot template | none | post-merge assert |

The second row is the strategic win for v0.4.x+: kernel drivers that map
devices late (drivers.md FLAW-06-era virtio work) stop being fork-ordering
hazards.

## 5. Test Plan

New class `pt_merge` (mirroring test_pml4_clone.cpp style):
1. `merge_equivalent_to_full_copy` — random kernel-half population; merge
   result byte-equal to deep-copy baseline (canonical semantics).
2. `merge_idempotent` — double merge, zero page delta.
3. `late_mapping_visible_after_merge` — add PDPT entry post-template,
   fork+merge, child sees it; pre-merge child does not.
4. `oom_mid_merge_cleans_up` — inject failure at level 2; child pages fully
   freed (zero delta), caller gets -ENOMEM.
5. `template_mismatch_panics` — corrupt child entry, expect debug-mode panic.

Regression gate: existing fork/process tests (process 43/43) unmodified.

## 6. Non-Goals

- COW/fork-light (shared user frames) — separate decision, not implied here.
- Removing the by-value copy of HHDM leaves in *existing* tables — merge
  changes creation-time behavior only.
