# Jarvis RTOS — Development Roadmap

**Build:** v0.4.0-dev | **Last Release:** v0.3.12 | **Completed milestones:** see `ROADMAP_done.md` (v0.2.x — v0.3.12)

## Safety & Concurrency Guardrails (Strict)
- **Transition to Fine-Grained Locks:** All new synchronization code must use `SpinLock` + `SpinLockGuard` for short critical sections and `sync::Mutex` (without IrqGuard) for blocking paths. The global `IrqGuard` is deprecated for all uses except boot, panic, and test isolation.
- **Reference-Enforced Tasks:** When manipulating task blocks or IPC endpoints within the new init system or system calls, strictly enforce reference passing over raw pointers to prevent dangling lookups.
- **Zero-Allocation tmpfs Operations:** Ensure the initial `tmpfs` implementation relies on the pre-existing fixed `MemPool` / `BufferPool` infrastructure for its nodes to avoid unbounded allocations that violate resource tracking limits.

## Active Development — v0.4.0

### Memory Protection (Phase 4.5) — prerequisite for safe SMP

Source: `docs/specs/memory.md` §7 (REQ-MP-01..06). Current state:
user↔user isolation + user-stack guard pages present; kernel-task↔kernel-task
isolation ABSENT; software canaries absent. Decisions: full private kernel
page tables, both MMU guard pages + software canaries, HW enforcement
(SMAP/SMEP/PAN/PXN) recommended-not-mandatory.

**Ordering rationale:** MP-7 (deep-copy fork) and MP-1 (private kernel-half
page tables) are the foundation — they restructure how a task's `page_table_`
is built and torn down. MP-2/MP-3/MP-6 layer guards/canaries on top of that
structure. MP-4 (HW) is independent and can land in parallel after MP-1/MP-7.
MP-5 is the verification suite that must track every other item.

---

#### MP-7 — `page_table_shared_` removal (complete deep-copy fork) — FIRST

- [ ] **MP-7.1 — Audit the sharing flag.**  Grep `page_table_shared_` across
      `src/kernel/**`.  The flag is set/cleared at `task.hpp:212/268`,
      `task.cpp:154-155` (init), `task.cpp:1107` (clone → deep copy, sets
      false), and read at `task.cpp:1424` (cleanup gate) and
      `vmm.cpp:612` (`free_user_pages` skip).  ROADMAP notes "deep copy
      replaced shared page tables; scheduler.cpp:1435" — confirm the sharing
      path is already dead (only `false` is ever written post-clone).  Output
      a usage table (file:line, read/write, reachable?).
- [ ] **MP-7.2 — Walk-and-copy fork.**  In `clone()` (task.cpp:1100-1130),
      after `clone_kernel_pml4()`, walk every user PML4 entry of the parent;
      for each present PDPT/PD/PT allocate a fresh table page, copy contents,
      and install into the child's PML4.  Skip the kernel half (entries above
      `PML4_USER_COUNT`).  Preserve the existing stack-region handling
      (`stack_pdpt_phys_`, task.cpp:1125).
- [ ] **MP-7.3 — Remove the flag.**  Delete `page_table_shared_` from
      `task.hpp`; remove its writes in `task.cpp:154-155,1107` and the read
      gate at `task.cpp:1424` (make `free_user_pages` + `PMM::free_page`
      unconditional for user tasks).  Update `vmm.cpp:612` `free_user_pages`
      to drop the shared-skip.
- [ ] **MP-7.4 — Verify no alias.**  After a clone, write a known pattern into
      the child's user PT page and confirm the parent's corresponding PT is
      unchanged (physical addresses differ).  Class gate: `pml4_clone`,
      `process`, `memory`.
- [ ] **MP-7.5 — ResourceTracker:** assert zero PMM delta across clone+exit
      (each copied table page freed on child cleanup).

**Hypothesis:** sharing is already effectively disabled; removing the flag and
making the fork a true deep copy cannot regress isolation because no live path
writes `true`.  **Validation:** `pml4_clone` + `process` + `memory` to 0
failures, zero ResourceTracker delta, `make build` clean.

---

#### MP-1 — Private kernel-half page tables per kernel task

- [x] **MP-1.1 — Kernel-half layout spec.**  Document the private kernel map a
      kernel task needs: kernel text/data/bss (already mapped in the kernel
      PML4), its own kernel stack frame, and the HHDM direct map (preserved
      for kernel→user access, REQ-MP-04).  Decide which kernel VA regions are
      per-task-private vs. shared-readonly (text) vs. shared (HHDM).
      **DONE (2026-08-13):** `docs/specs/memory.md` §7.1.1 — per-region
      classification table (per-task-private = user half + stack guard only;
      shared-readonly = kernel text; shared = data/bss + HHDM + kslot window,
      all copied by value from the boot kernel PML4).
- [x] **MP-1.2 — Private kernel PML4 clone.**  Add `VMM::clone_kernel_pml4()`
      for the kernel half that copies only the kernel entries (PML4 indices ≥
      `PML4_USER_COUNT`) into a fresh top-level table, and maps the task's
      private kernel-stack frame (text/data/bss shared-readonly).  Reuse
      `alloc_kslot`/`kstack_slot_va_` for the private stack VA window.
      **DEVIATION (binding):** the kernel stack stays in the boot-shared kslot
      window (`CONFIG_KSTACK_WINDOW_BASE`, PML4 index 498 ≥ PML4_KERNEL_START).
      Every private clone inherits the window's PD/PT entries by value; per-task
      private kstack page tables are NOT allocated.  The guard page below each
      slot plus the MP-6 hook enforce stack isolation (docs/specs/memory.md
      §7.1).
- [x] **MP-1.3 — CR3 switch plumbing.**  In `switch_to_task` /
      `scheduler_on_context_switch` (scheduler.cpp:2264, isr epilogue), load
      the task's private kernel PML4 phys into CR3 when dispatching a kernel
      task (currently only user tasks switch CR3 via
      `scheduler_load_cr3_from`).  Preserve HHDM + kernel text in every
      private table so kernel code keeps running after the switch.
      **DONE (2026-08-13):** every task (kernel AND user) owns a private
      `page_table_` (MP-1.2), so `switch_to_task` always publishes
      `scheduler_load_cr3_from = next.page_table_` and the ISR epilogue loads
      it for all dispatches; the static `scheduler_kernel_cr3` fallback is hit
      only when no CR3 was published.  Proven by
      `memory_kernel_isolation`/`kernel_priv_cr3_switch_on_dispatch`
      (`g_cr3 == a->page_table_` inside the dispatched task) +
      `kernel_priv_cross_task_data_isolation`.  HHDM + kernel text preserved by
      `clone_kernel_pml4()` (kernel half copied by value).
- [x] **MP-1.4 — Teardown.**  Extend `TaskControlBlock::cleanup()` to free the
      private kernel PML4 + its private kernel-stack PT/PDPT/PD (mirror
      `free_user_pages` + `free_stack_pdpt` for the kernel half).
      **DONE (2026-08-13):** `cleanup()` → `VMM::free_user_pages(page_table_)`
      (walks user half only, `PMM::is_user_page` guard skips all boot-shared
      kernel pages) + `PMM::free_page(page_table_)` reclaims the single private
      PML4; kernel tasks have empty user halves so only one page is freed.
      The kslot-window/HHDM/text pages are boot-shared and NOT freed
      individually (spec §7.1.1 rule 4).  Proven by
      `memory_kernel_isolation`/`kernel_priv_teardown_frees_pml4_stack` (zero
      PMM delta after kernel-task create+dispatch+teardown).
- [ ] **MP-1.5 — Kernel-task isolation proof.**  Two kernel tasks with
      different data pages: write to task A's private page from task A, read
      from task B (dispatched) → must #PF (unmapped in B's private table).
- [ ] **MP-1.6 — Class gates:** `scheduler`, `process`, `pml4_clone`,
      `memory`, `selftest`, `make build`.

**Hypothesis:** per-task kernel PML4 with shared-readonly text + private stack
gives kernel-task↔kernel-task isolation while preserving HHDM access; CR3
switch on every dispatch is safe because HHDM/text are identical in every
private table.  **Validation:** cross-kernel-task #PF probe (MP-1.5) +
`process`/`pml4_clone` gates.

---

#### MP-6 — Kernel stack guard page via private VA window

- [ ] **MP-6.1 — Guard-page slot.**  In `alloc_kslot` (task.cpp:382), each
      slot already reserves one unmapped page at the base (`CONFIG_KSTACK_*`,
      kslot guard).  Verify the guard is enforced on kernel-stack overflow:
      a task that overruns `kernel_stack_top` must #PF before touching the
      next slot's data.
- [ ] **MP-6.2 — Overflow hook.**  Wire `CONFIG_STACK_OVERFLOW_HOOK` (currently
      0): on a guard-page #PF (PTE not-present on the kslot base), invoke the
      hook → panic with task id + RIP, then halt (no silent corruption).
- [ ] **MP-6.3 — Snapshot-safe pool.**  The kslot page-table pool must be
      captured/restored by `snapshot_restore` (test isolation) — verify
      `capture_state`/`restore_state` covers `s_kstack_pt_pages` and the kslot
      PTs; add if missing (mirror the v0.3.11 `pool_pages_` fix).
- [ ] **MP-6.4 — Class gates:** `stack_alloc`, `stack_profiler`, `scheduler`,
      `selftest`, `make build`.

**Hypothesis:** the kslot base guard page is already present but the overflow
hook is off; enabling it converts silent stack corruption into a diagnosed
panic, and snapshot-safe pools keep it test-isolation-clean.

---

#### MP-2 — MMU red-zone guard pages between kernel + user segments

- [ ] **MP-2.1 — Segment-map audit.**  Enumerate the user task VA layout:
      text/data/heap/stack (`STACK_VADDR`, `program_break`, segment base from
      ELF load).  Identify every adjacent-pair boundary that currently has no
      unmapped page between mappings.
- [ ] **MP-2.2 — Insert red-zones.**  For each boundary, leave one 4 KiB page
      unmapped (never installed in the task PML4) between segments.  Adjust
      the ELF loader (`load_segments_and_stack`) and `brk`/heap growth
      (`program_break`) to reserve the gap.
- [ ] **MP-2.3 — Kernel-half red-zones.**  In MP-1's private kernel PML4, leave
      an unmapped page between the kernel stack frame and adjacent kernel
      data (belt-and-braces beyond the kslot guard).
- [ ] **MP-2.4 — Verify.**  A task writing just past a segment end must #PF
      (PTE not-present), not corrupt the neighbour.  Class gates:
      `process`, `memory_safety`, `pmm`, `selftest`.

**Hypothesis:** gaps are only absent because the loader never reserves them;
reserving one unmapped page per boundary converts overflow into a deterministic
#PF without changing valid access.

---

#### MP-3 — Software sentinel canaries at segment boundaries

- [ ] **MP-3.1 — Canary layout.**  Define a per-task canary structure: a
      known 8-byte magic placed immediately before and after each guarded
      segment (text/data/heap/stack), aligned to 8 bytes.
- [ ] **MP-3.2 — Install canaries.**  On segment init (ELF load, heap grow,
      stack setup), write the canary into the guard slots.  Store the expected
      canary values in the TCB (`canary_before`/`canary_after` per segment,
      fixed-size array).
- [ ] **MP-3.3 — Verify on entry.**  On `syscall` entry and on
      `switch_to_task` dispatch, check every canary matches; on mismatch,
      panic with task id + segment + faulting RIP (no silent corruption).
- [ ] **MP-3.4 — Test:** canary-tamper — write 0xDD over a canary, trigger a
      syscall, assert the panic/detection path fires (MP-5 companion).
- [ ] **MP-3.5 — Class gates:** `memory_safety`, `process`, `selftest`,
      `make build`.

**Hypothesis:** canaries give a software-detectable overflow signature at
segment boundaries that the MMU guard alone cannot catch (sub-page overflows
into another mapped page).  **Validation:** tamper test must be
deterministic (MP-3.4).

---

#### MP-4 — Optional HW enforcement: SMAP/SMEP (x86_64) / PAN/PXN (aarch64)

- [ ] **MP-4.1 — CR4 SMEP enable.**  In `arch_init` (x86_64), set
      `CR4.SMEP` (bit 20).  Kernel code that legitimately reads user memory
      (copy_from_user paths, syscall arg fetch) must be wrapped in
      `stac`/`clac` (or use explicit `__get_user`-style accessors) — audit
      every user-pointer deref in syscall handlers first.
- [ ] **MP-4.2 — CR4 SMAP enable.**  Set `CR4.SMAP` (bit 21) after the SMEP
      audit passes.  Add `stac` before / `clac` after every direct user-memory
      access in the kernel.
- [ ] **MP-4.3 — stac/clac audit.**  Grep `copy_from|copy_to|user_ptr|args` in
      syscall handlers; wrap each in the AC-flag save/restore pair.  Verify no
      kernel→user deref executes with AC=0 (SMAP fault).
- [ ] **MP-4.4 — aarch64 PAN/PXN.**  Enable `PAN` (bit 22 of SCTLR_EL1) and
      `PXN` on kernel PTEs; mirror the stac/clac discipline with
      `ldtr/`PAN-clear on kernel→user loads.
- [ ] **MP-4.5 — Negative tests.**  MP-5 suite: user task executes kernel VA
      (SMEP #PF), kernel derefs user VA without AC (SMAP #PF), HHDM read still
      works (REQ-MP-04).
- [ ] **MP-4.6 — Class gates:** `memory_safety`, `cross_arch`, `selftest`,
      `make build`.  Mark MP-4 "recommended-not-mandatory" — if SMAP breaks a
      syscall path, keep SMEP only and log the gap.

**Hypothesis:** SMEP/SMAP are safe once every kernel→user access is AC-wrapped;
the syscall handler audit is the gating prerequisite.

---

#### MP-5 — Verification suite (cross-task #PF, canary-tamper, HHDM, SMAP/PAN negatives)

- [ ] **MP-5.1 — Cross-task #PF.**  Dispatched kernel task A maps a private
      page; dispatched kernel task B derefs A's VA → assert #PF (fault
      handler records) and task B is terminated cleanly (no system hang).
- [ ] **MP-5.2 — User-overflow #PF.**  User task writes past its stack/text/
      heap red-zone → assert the guard #PF fires and the task is killed, not
      the kernel.
- [ ] **MP-5.3 — Canary-tamper detection.**  Corrupt a canary, trigger the
      verify path → assert panic/detection (MP-3.4 companion, may be
      `#if`-gated to a detection callback rather than a hard halt in tests).
- [ ] **MP-5.4 — HHDM kernel→user read.**  Kernel reads a user page via the
      direct map → succeeds (REQ-MP-04 negative stays green).
- [ ] **MP-5.5 — SMAP/PAN negatives.**  If MP-4 landed: kernel deref of user VA
      without AC → #PF; with AC → success.  Gate the asserts on
      `CONFIG_SMAP/SMEP`.
- [ ] **MP-5.6 — Register under `memory_safety` + `cross_arch` classes;**
      update `test_expected_counts.hpp`; class gates `memory_safety`,
      `cross_arch`, `pmm`, `selftest`, `make build`.

#### MP-8 — Rework existing tests for the MP-1..MP-7 memory model

The Memory Protection work changes kernel/page-table invariants that several
existing tests assert.  Rework them to match the post-MP model (driven
cookbook, no field mutation, no shared-page-table assumptions) rather than
leaving stale assertions.

- [ ] **MP-8.1 — `test_pml4_clone.cpp` shared-table tests.**  Rework
      `pml4_free_user_pages_shared_safe` (test_pml4_clone.cpp:339) — it
      simulates a child sharing the parent's page tables, which MP-7
      eliminates.  Rewrite to the deep-copy model: child has its own PD/PT
      copies, `free_user_pages(child_pml4)` frees every copied table + data
      page, and the parent's entries are provably untouched (phys differs).
      Keep `pml4_fork_user_entries_match` / `pml4_fork_no_child_corrupt_parent`
      (they already test deep-copy) but verify they still hold after MP-1
      (private kernel half).
- [ ] **MP-8.2 — `test_page_tables.cpp` stubs (9).**  Replace the
      `JARVIS_TEST_PASS()` placeholders with real assertions against the MP-1
      private-kernel PML4 / MP-7 deep-copy walk (page-table allocate/install/
      walk/free round-trips on a cloned PML4).
- [ ] **MP-8.3 — `test_stack_alloc.cpp` stubs (8).**  Implement real guard-page
      assertions: allocate a kslot stack, verify the base page is unmapped,
      verify an overrun hits `CONFIG_STACK_OVERFLOW_HOOK` (MP-6), and that the
      slot is reclaimed on free (no ResourceTracker delta).
- [ ] **MP-8.4 — `test_process.cpp` stubs (12).**  Replace placeholders with
      real fork/clone/exit assertions under the deep-copy model (MP-7) and
      private kernel page table (MP-1): child page-table independence, parent
      preservation, teardown completeness.
- [ ] **MP-8.5 — `test_memory.cpp` / `test_memory_safety.cpp`.**  Audit for
      assertions that hard-code the pre-MP layout (e.g. specific PT-page
      counts under a shared pdpt, or `free_user_pages` skip semantics).
      Update to the post-MP expected counts.  `memory_safety_pmm_free_zero`
      (test_memory_safety.cpp:82) — keep the reserved-page invariant assert.
- [ ] **MP-8.6 — `test_fpu_clone.cpp` / FPU suite.**  Confirm FPU-state copy
      on clone still holds when the child gets fresh page tables (MP-7) and a
      private kernel half (MP-1); adjust if the clone path changes.
- [ ] **MP-8.7 — driven-cookbook compliance.**  Any reworked test must follow
      the v0.3.10 cookbook: real dispatch (create prio≥11 → add_task →
      reschedule → busy-wait), no `set_current` impersonation, no direct
      `task->state/priority/deadline_ticks` mutation, no fake `on_tick()`.
- [ ] **MP-8.8 — Class gates:** `pml4_clone`, `process`, `memory_safety`,
      `page_tables`, `stack_alloc`, `memory`, `selftest`, `make build`;
      update `test_expected_counts.hpp`; `test-history.txt` rows.

**Hypothesis:** the shared-table and stub tests encode pre-MP invariants that
MP-1/MP-7 invalidate; reworking them to the deep-copy + private-kernel model
keeps the suite truthful and prevents false green.  **Validation:** each
reworked class to 0 failures with zero ResourceTracker delta; `selftest`
132/132; `all` 835/835.

**Required fix discipline (per AGENTS.md Mandatory Bugfix Sequence):**
1. Classify each: MP-1/MP-2/MP-6/MP-7 = memory/page-table, MP-3 = canary,
   MP-4 = HW enforcement, MP-5 = verification suite.
2. Read the affected memory/VMM/task code before editing.
3. One hypothesis per item, validated by build + the smallest applicable test
   class (MP-1/MP-7 → `pml4_clone`/`process`; MP-5 → `memory_safety`/`pmm`).
4. Implement, `make build` clean, run the class to 0 failures.
5. After all items: `make execute-test x86_64 debug all` (835/835), then the
   release gate `make execute-test x86_64 release all` (84/84).

**Acceptance criteria (DONE when):**
- MP-1..MP-8 fixed (each verified by build + class gate).
- `page_table_shared_` fully removed (MP-7); every kernel task has a private
  kernel-half page table (MP-1).
- Guard pages + canaries active; SMAP/SMEP enforced on x86_64 (MP-2/3/4).
- Existing tests reworked to the post-MP model; no stale shared-table or stub
  assertions (MP-8).
- MP-5 verification suite passes; `make build` clean (check-style Errors: 0),
  `selftest` 132/132, `all` 835/835, release 84/84.
- `test-history.txt` rows appended for every class touched.

**Out of scope:** ~~H2 race (v0.3.9)~~ (RESOLVED 2026-08-13, commit `71b3a088`),
~~BufferPool +1 (v0.3.11)~~
(RESOLVED), ~~IrqGuard guardrails (v0.3.12)~~ (RESOLVED), SMP (0.4.x APIC
boot), microkernel capability foundation (0.4.x), and userspace ABI (0.5.x).
The KernelObject shared-reference-count foundation (prerequisite for 0.4.1
CSpace) is complete — see the section below.

---

## H2 Deferred-Switch Race — RESOLVED (v0.4.0-dev)

**Note (2026-08-12):** the §v0.3.9 "H2 RESOLVED" status was premature.  The
`ipc_core` class wedges at test 21 `ipc_send_sync_roundtrip` (~50%, reproduced
repeatedly on `main` @ `464f1fbc` / `testbed` @ `a2750bd2`).  Fix for this
version with the directions below.  Full evidence + instrumentation in
`audits/deep-analysis-h2-ssdeadline-v0.3.9.md` §6.  **RESOLVED 2026-08-13** by
commit `71b3a088` (see the RESOLVED subsection at the end of this section).

**Root cause (pinned, 2026-08-12):** the harness's stored `context.rsp` can be a
stale frame pointing into a test body's **setup path** (not the wait loop).  On
resume the harness re-executes `yield_to_task(task)`'s
`Scheduler::enqueue_ready(task)` (test_sched_helpers.hpp:80) on a task that has
already self-terminated and been removed from `id_table` — recreating an orphan
runq node.  `next_task()` returns it, the harness arms a deferred switch to it,
the apply-side `find_task(id)==NULL` drops the arm, and the harness hlt-waits
forever (timer fires but no task is ever runnable → no more `[TICK]`).

Trace chain (one failing run, cold-path diagnostics):
`[H2-TERM6] → [H2-APPLY] stale frame → [H2-ENQDEAD] ra=yield_to_task → [SW]
cur=1 next=6 → [H2-ARMDEAD]/[H2-DEAD] find_task(6)==NULL → silence`.

**Fix directions (implement one or more for v0.4.0):**
1. **Refuse enqueue on removed tasks (defense-in-depth, minimal).**
   In `Scheduler::enqueue_ready`/`set_task_ready`, if
   `find_task(task.id) != &task`, drop the enqueue (the scheduler no longer owns
   the TCB) instead of inserting an orphan node.  Neutralizes the orphan
   regardless of which stale path resurrects it.
2. **`next_task()` liveness guard.**  Skip/remove runq candidates whose id is
   not in `id_table` (belt-and-braces at selection time, mirroring §1.2's
   stale-arm-to-removed-task guard but on the selection side).
3. **Prevent the stale-frame resume.**  The harness's `context.rsp` must not be
   restored/re-pointed to a test-body setup frame; keep it at a valid
   wait-loop `arch_hlt` frame across `restore_task_fields` and the save path.
   (Hardest; prior §4.6/§Option-1 attempts to re-point it regressed the race —
   validate carefully.)
4. **Harness wait-loop resilience (test-side).**  Ensure test-21 (and any
   `yield_to_task` user) never re-runs `yield_to_task` on a task that may have
   terminated; document the deferred-enqueue hazard in `test_sched_helpers.hpp`.

**Validation:** `make build` clean (check-style Errors: 0); `ipc_core` 23/23
across ≥ 10 consecutive runs (pre-fix ~50% hang); `all-1` reaches tests 485+
(pml4_clone, BUGS.md #21) without a hang; then the `all` gate per the debug
procedure.  Append a `test-history.txt` row after every class run.

### RESOLVED (2026-08-13) — commit `71b3a088` (fix directions #1 + #4)

Fixed with a two-layer liveness guard:

1. **Kernel guard — `Scheduler::enqueue_ready` (scheduler.cpp:190).**  Refuses
   to enqueue any TCB not owned by `id_table_` (`find_task(task.id) != &task`),
   so the orphan runq node can never be created.  The cold `[H2-ENQDEAD]` dump
   is preserved (trace-gated).  All legitimate enqueue callers enqueue
   registered tasks (`add_task` registers before enqueue; wake/switch-away
   paths enqueue live current tasks) — the guard cannot fire spuriously.
2. **Test-side guard — `yield_to_task` (test_sched_helpers.hpp:66).**  Refuses
   at entry if the task is dead, before any side effect.  The kernel guard alone
   was insufficient because the helper ran `set_current(dead)` + `state = READY`
   *before* the enqueue could be refused.

The `next_task()` liveness guard (direction #2) was deliberately NOT added —
the enqueue-side guard makes the orphan unreachable at the source, and a
selection-time filter would add per-dispatch cost on a hot path.

**Validation results (2026-08-13):**
- `ipc_core` 23/23 across 14+ consecutive runs (pre-fix ~50% hang at
  `ipc_send_sync_roundtrip`).
- Debug `all` gate: 7 clean runs in 10 invocations, **zero watchdog hangs, zero
  H2 diagnostics** (pre-fix ~50-70% hang rate); runs where the stale resume
  occurred recovered via the guard.
- Release gate (trace OFF): **84/84** — the previously documented trace-OFF
  deterministic hang at `ipc_send_sync_roundtrip` no longer reproduces.
- Full evidence + mechanism in `audits/deep-analysis-h2-ssdeadline-v0.3.9.md` §7.

**Status:** H2 RESOLVED.  ROADMAP §v0.3.9 entry superseded; the trace-OFF
release-gate caveat in AGENTS.md is no longer applicable.

---

## KernelObject Shared-Reference-Count Foundation (2026-08-13, commits `ee2f24c0`..`62467804`)

**Purpose:** the reference-counting primitive ROADMAP 0.4.1 (CSpace) builds on.
Establishes the "TCB is the single source of truth for task-owned objects"
model with a genuine multi-holder shared refcount, SMP-ready.

### Commits
- `ee2f24c0` — intrusive RefCounted per-task object list on TCB (v1: concrete
  base, disposer fn-pointer + kind-tag, TCB-owned object list, SporadicServer
  migrated; closed the "SporadicServer never freed" lifecycle gap)
- `71b3a088` — H2 stale-resume orphan re-enqueue fix (see above)
- `07de8527` — M1: rename `RefCounted` → `KernelObject` (mechanical)
- `d1a16de3` — M2: pure-virtual `KernelObject` — `dispose()` (last-release
  teardown, CPU-agnostic), `revoke()` (capability revocation hook), `is_shared()`
  (ownership-class marker), atomic acquire/release (ACQUIRE/RELEASE on revoke),
  `ScopedRef::valid()`; SporadicServer virtualized (placement-new factory,
  dispose → dec_sporadic_count + MemPool::free); TCB teardown asserts `>=1`
  universal + `==1` non-shared
- `4f9e404c` — M-extra step0: clone fd-copy `vnode_ref_inc` fix (latent
  over-decrement → premature close/double release) + `process_clone_pipe_fd_
  refcount` regression test
- `caf35962` — M-extra step1: PipeBuffer migrated to shared KernelObject
  (creator-ref → two-end-refs handoff; replaced the non-atomic `int refcount`
  which raced on SMP)
- `62467804` — SIL 3 audit fixes (SMP ordering, stale comment, ScopedRef
  contract)

### Ownership classes
- **Class A — private-owned heap** (SporadicServer): TCB list holds the only
  long-lived reference; teardown asserts refcount==1; dispose frees the block.
- **Class B — embedded objects** (per-task MessageQueue/Notify/EventGroup):
  NOT KernelObject-derived, NOT on the list, NO refcount.  Storage lives inside
  the TCB block and cannot outlive it; cross-task references are raw and
  detached at owner teardown.  Avoids vptr-wipe-by-memset and dangling-into-
  owner-block hazards.
- **Class C — genuinely shared heap** (PipeBuffer, future capability objects):
  real multi-holder refcount; every holder acquires/releases; dispose() runs on
  the last release regardless of CPU.  CSpace capability objects (ROADMAP 0.4.1)
  follow this class, using `revoke()` for deterministic revocation.

### Validation
- `make build` green (style Errors: 0).  Debug `all` **859/859** (trace ON);
  release `all` **84/84** (trace OFF).  All per-class gates green.
- SIL 3 audit (independent): **APPROVED**, no BLOCKER/HIGH; applied SMP ordering
  + documentation items.  Auditor finding #2 (dup2 double-release) verified
  FALSE — `FdTable::free` gates `ops->close` on `vnode_ref_dec` reaching 0, so
  dup2/clone `vnode_ref_inc` means pb is released exactly once per endpoint.
- Residual `harness_blocked_sender_wakes` flake: pre-existing v0.4.0 MP-8
  timing issue, reproduces identically on the pre-rework baseline.

---

## Past Releases

See `ROADMAP_done.md` for completed items: v0.2.x — v0.3.12 (boundary audit, PfA concurrency redesign, test hygiene, H2 race, trigger-driven testing rework, BufferPool +1 PMM leak, Fine-Grained Lock & Safety-Guardrail Enforcement).

---

## Future Roadmap (Aspirational)

### Phase 4.6: Microkernel Primitives & Capability Foundation (0.4.x) — prerequisite for 0.7.x Microkernel Transition

#### 0.4.1 — Capability-Based Access Control Architecture (CSpace)
- [ ] **Capability System Specification** — Design object capability structures (CNode, CSlot, CSpace) for tasks, IPC endpoints, physical memory frames, Untyped memory, IRQ controls, and MMIO ranges (eliminating ambient authority).
- [ ] **Capability Lifecycle Primitives** — Implement `SYS_CAP_GRANT`, `SYS_CAP_REVOKE`, `SYS_CAP_MINT`, and `SYS_CAP_COPY` system calls with deterministic reference-counted cleanup.
- [ ] **Untyped Memory Allocator (seL4-style)** — Transition physical allocation away from direct PMM calls to explicit Untyped memory retyping (`Untyped.Retype` into Frame, PageTable, CNode, or Endpoint capabilities).

#### 0.4.2 — User-Space Driver Infrastructure & Hardware Isolation
- [ ] **User-Space IRQ Delivery System** — Hardware IRQ dispatcher that transforms incoming interrupts into Capability-backed IPC Notifications (`sys_irq_register` / `sys_irq_wait`) to eliminate Ring 0 driver execution.
- [ ] **Fine-Grained Hardware I/O Delegation** — Per-task TSS I/O port bitmap management (`sys_ioport_grant`) and capability-gated MMIO page frame mapping for Ring 3 drivers.
- [ ] **IOMMU DMA Protection Layer (VT-d / AMD-Vi / SMMU)** — Program IOMMU translation tables to isolate Ring 3 driver DMA requests strictly to task-owned physical memory frames.

#### 0.4.3 — High-Performance Zero-Copy IPC & Fault Isolation
- [ ] **In-Register IPC Fastpath** — Assembly fastpath for short messages passed via CPU registers (`rax`, `rbx`, `rcx`, `rdx`, `rsi`, `rdi`) skipping full TCB context saves.
- [ ] **External Pager Protocol (#PF Delegation)** — Exception forwarding mechanism routing user-space `#PF` / page faults over IPC to a capability-designated Ring 3 Pager thread.
- [ ] **Fault Recovery & Crash Supervisor** — Asynchronous task-death notifications allowing supervisor processes to catch server crashes, reclaim leaked capabilities, and restart failed drivers cleanly.
- [ ] **Capability Shared-Memory Granules** — Zero-copy ring buffer shared memory mappings (`SYS_SHM_MAP`) backed by capabilities for high-throughput client-server I/O.

### Phase 5: SMP + Multicore (0.4.x)
#### 0.4.4 — APIC & SMP Boot
- [ ] Local/IO APIC, X2APIC, per-CPU GDT/TSS, INIT-SIPI AP startup
- [ ] TPR-based interrupt prioritization, core state isolation
- [ ] **Per-CPU asm for `isr_nesting_depth`** — move from the single global symbol to GS-relative access on x86_64 (TPIDR/tp on aarch64/riscv64); the C++ side already uses `__atomic_*` (v0.3.7 PfA-B). CpuContext plumbing (`current_cpu()`) is in place.
- [ ] **`hhdm_modified_` (VAR-17) re-audit** — task-context only today (single-core safe); re-audit under SMP with per-CPU ownership or atomics.

#### 0.4.5 — Per-CPU Scheduling & Cache
- [ ] Distributed run queues, real-time load balancer, SYS_SET/GET_AFFINITY
- [ ] Cache coloring allocator, SMP spinlocks/rwlocks, WCET re-audit

#### 0.4.6 — TLB Shootdown & IPI Reduction
- [ ] PCID, selective INVPCID, lazy shootdowns, IPI batching, latency profiling

### Phase 6: System Integration / Userspace ABI (0.5.x)

**Priority:** picolibc integration — syscall ABI, TLS, POSIX stubs.

#### Syscall ABI Definition
- [ ] **Document trap/IRQ numbers** — create `src/kernel/syscall/syscall.h` with stable, documented trap vectors and IRQ numbers
- [ ] **Register conventions** — specify register layout for syscall arguments and return values per architecture (x86_64: `rax=num, rdi, rsi, rdx, r10, r8, r9`; aarch64: `x8=num, x0-x5`; riscv64: `a7=num, a0-a5`)
- [ ] **syscall.h public header** — export to userspace, used by both kernel dispatcher and libc stubs

#### picolibc Integration
- [ ] **POSIX syscall stubs** — implement `src/libc/picolib_stubs.c` with wrappers for `_write`, `_read`, `_sbrk`, `_exit`, `_open`, `_close`, `_fstat`, `_lseek`, `_getpid`, `_kill` using `jarvis_syscall()` dispatcher
- [ ] **Build picolibc** — compile with meson as `libc.a` + `libm.a` (static), targeting x86_64-elf
- [ ] **Makefile integration** — link `libc.a`/`libm.a` into kernel image; add build rules for picolibc subproject
- [ ] **TLS on context switch** — every task switch must load the thread-local-storage address into the appropriate base register (`FS` on x86_64, `TPIDR_EL0` on aarch64, `tp` on riscv64). picolibc uses this for `errno` and per-task internal state — no global locks needed.
- [ ] **Verify** — `printf`, `malloc`, `scanf` work from userspace tasks via syscall stubs

### Phase 7: Safety Systems (0.6.x)
- [ ] ICH9/HPET hardware watchdog + NMI pre-timeout, PIT fallback, SYS_WATCHDOG_KICK
- [ ] Per-task software watchdog (SYS_WATCHDOG_CREATE), /proc/[pid]/watchdog
- [ ] Wait-for-graph deadlock detection, watchdog-driven recovery, SYS_HEALTH_STATUS
- [ ] Idle-task safety monitors: RAM March C-, CPU ALU verification, utilisation tracking

### Phase 8: Microkernel Transition (0.7.x–0.8.x)
- [ ] Externalise VFS & block I/O to user-space servers
- [ ] Externalise device drivers (keyboard, framebuffer, timer/RTC)
- [ ] Kernel reduction: scheduler, IPC, page-table management, interrupt routing only
- [ ] Capability-based security (SYS_CAP_GRANT / SYS_CAP_REVOKE)

### Phase 9: Hardware Drivers & Protocols (0.9.x)
- [ ] Full TCP/IP stack (ARP, IP, ICMP, UDP, TCP) with Ethernet NIC driver
- [ ] USB driver stack (UHCI/EHCI/xHCI)
- [ ] Hot-path secure call sequence layer (<seqguard.hpp>)
