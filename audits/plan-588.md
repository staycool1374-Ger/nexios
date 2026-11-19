# PLAN: write-fault-safe waitpid status delivery (RO-mapped status page)

Issue: #588 (bug, severity S2, `process`). Base: `main` @ `b843afe78`
(dispatch-declared; this planner runs without a shell — anchors below were
re-verified by read against the working tree, which showed no evidence of a
dirty tree in the touched files). Production change → full pipeline incl.
Phase 3 auditor. Test files `src/kernel/test/**` are read-only (QE owns
`testbed`); this plan hands QE a sketch, it does not write tests.

## AFFECTED FILES
- `src/kernel/syscall/syscall_handlers_process.cpp` — `sys_waitpid`
  (`:59-163`): add the writability pre-cert, reorder it **before** the
  `waiting_child_pid` publication (`:132`), return the typed fault error.
- `src/kernel/task/scheduler_lifecycle.cpp` — `wake_waiting_parent`
  (`:389-468`): move the CR3-switch + store + CR3-restore into a new shared,
  fault-armed delivery helper; fix the false comment at `:412-416`.
- `src/kernel/syscall/syscall_handlers_misc.cpp` — `sys_exit`'s duplicate
  store (`:162-183`): replace the inline store with the same helper; fix the
  false comment at `:171-176`.
- `src/kernel/task/scheduler.hpp` — declare the helper next to
  `wake_waiting_parent` (`:738-745`) and correct that doc-comment.
- `src/kernel/memory/vmm.hpp` — declare the production walk predicate next to
  the existing RO primitives (`:298-318`); state its caller contract.
- `src/kernel/memory/vmm_map.cpp` — implement the predicate beside
  `pte_is_ro_in_pml4` (`:333-339`), reusing `pte_leaf_in_pml4` (`:280-310`)
  and `vmm_arch_leaf_is_ro` (per-arch TUs).
- `src/kernel/log/dmesg_catalog.hpp` — one new SCHED record at the first free
  slot (`kDmesgBase_SCHED + 20`, table at `:280-289`) + severity mirror in
  `lookup_severity`'s SCHED arm (`:416-420`).
- `docs/specs/scheduler.md` — append the wait-status delivery contract
  (present ≠ writable; pre-cert at entry + armed store at delivery) **only if**
  the waitpid/wake section exists there; otherwise the two header
  doc-comments are the record (step 13 verifies before editing).
- No new translation unit ⇒ `tools/meson/gen.mk` regeneration is NOT required
  (step 15 proves it).

## INVARIANTS & CONCURRENCY BOUNDARIES
- **Anchors (tree truth, verified by read):** store `scheduler_lifecycle.cpp:417-419`
  (`arch::stac(); *p->waiting_child_status = child.exit_code; arch::clac();`),
  CR3 switch `:405-411`, restore `:420-421`, pointer clear `:422`;
  duplicate store `syscall_handlers_misc.cpp:177-179`;
  mapped-only pre-cert `syscall_handlers_process.cpp:139-144`; publish order
  `:132` (pid) → `:139` (cert) → `:145` (status).
- **INV-WAIT-1 (must not weaken):** a user `status` pointer is written only
  after (i) `checked()` range validation (`:63-66`), (ii) a MAPPED
  pre-certification, and — new — (iii) a WRITABILITY pre-certification covering
  the entire 8-byte store window. Fail direction stays *return failure, mutate
  nothing*.
- **INV-WAIT-2 (currently violated, fix here):** the certs must run **before**
  `cur->waiting_child_pid = target_pid` (`:132`)/`dequeue_ready`/`state =
  BLOCKED`. Today the pid is published first, so a rejected status pointer
  leaves a stale `waiting_child_pid` on a READY task; a later
  `wake_waiting_parent` then spuriously re-`set_task_ready`s it. Reorder.
- **INV-WAIT-3 (unchanged):** the reap (`:122-127`) stays after the status
  write; a failed status copy leaves the zombie linked to a live parent
  (deferred by design) and reapable once the parent terminates.
- **Store context (S1-relevant):** `wake_waiting_parent` runs (a) inside
  `SchedulerLockGuard guard(scheduler_lock_)` — `scheduler_terminate.cpp:107-135`,
  (b) from the timer-tick watchdog KILL arm with no lock —
  `scheduler_watchdog.cpp:307-311` (lock-free by design, see `:347-353`),
  (c) from IRQ-masked fault/trap context (`aarch64/...interrupt_controller.cpp:318-321`).
  The delivery helper must therefore take **no lock, allocate nothing, and log
  only through the lock-free dmesg SPSC ring** (`dmesg.hpp:260-273`).
- **Single-slot recovery contract** (`docs/specs/external-pager.md:366-384`):
  `g_user_access_recover_ip` is checked before everything and must be **cleared
  on exit**; the pager's delegation precondition F2 requires it to be `0`. A
  nested arm (ISR inside a copy window) must therefore be
  **saved and restored**, not blindly zeroed.
- **Same-frame rule** (`checked_ptr.hpp:36-51`, issue #149/#177): the recover
  label must live in the frame that executes the store, with
  `NEXIOS_FAULT_RECOVERY_KEEP` in reachable code before the label and no
  non-vacuous initialization crossing it. Coverage builds use `-fno-inline`,
  so the store must stay a direct dereference inside the helper.
- **AC discipline:** `arch::clac()` must be the first statement of the recover
  label — a leaked AC panics debug/x86 at syscall entry
  (`syscall.cpp:150-159`, "MP-4: AC flag leaked") and on release leaks SMAP
  bypass to ring 3 via `sysret`.
- **CR3 discipline:** the armed store runs under the **parent's** page table
  (`:405-411`); the recover label must sit **before** the CR3-restore code so
  both paths converge on restoring the caller's CR3 (`:420-421`).
- No dynamic allocation, no new task/IPC/VFS resource ⇒ ResourceTracker delta
  must stay 0 (walk-only page-table reads).

## STEP-BY-STEP CHANGES

**Step 0 — retrieval artifacts (check-7, already executed; coder pastes these
plus dispositions into the issue work-begun comment):**
- graphify `query_graph("waitpid blocked parent status pointer write during
  wake; what code paths store to a user pointer from the scheduler?")` → used:
  confirmed the `Scheduler`/`find_task`/`set_task_ready` neighbourhood and the
  absence of any other scheduler-side user store.
- graphify `query_graph("fault recovery g_user_access_recover_ip user copy
  stac clac SMAP")` → used: surfaced `docs/specs/external-pager.md` §6.1/§6.2
  (single-slot + AC window) which governs the arm/restore design.
- graphify `shortest_path("Scheduler::wake_waiting_parent",
  "g_user_access_recover_ip")` → **no path** (ignored: negative result,
  documents that no recovery link exists today — the hazard).
- obsidian `search_vault("wake_waiting_parent")` → used:
  `audits/plan-586.md:76,79,90` (origin of this issue) and
  `audits/report-2026-09-30T15-39-08Z.md` (wake exactly once inside
  `terminate_err`). `search_vault("SMAP AC windows")` and
  `search_vault("pre-cert mapped but not writable")` → **0 matches**
  (ignored: no prior vault precedent, so the spec + code precedent is the
  authority).

**Phase 1 — writability predicate (VMM, all three arches)**
1. `src/kernel/memory/vmm.hpp`: declare
   `[[nodiscard]] static bool user_write_window_writable(uint64_t virt_addr,
   uint64_t bytes, uint64_t pml4_phys);` beside `pte_is_ro_in_pml4`, with a
   doc-block stating: walk-only, never allocates, no lock; returns true only
   when **every** byte in `[virt_addr, virt_addr+bytes)` lands on a present,
   writable 4 KiB leaf of `pml4_phys`; returns false for holes, RO leaves,
   huge/block leaves and overflow (fail closed). Mark it as a **production**
   query (the existing `pte_is_ro_in_pml4` keeps its test-inspector label).
2. `src/kernel/memory/vmm_map.cpp`: implement it next to `pte_is_ro_in_pml4`
   using `pte_leaf_in_pml4` (non-null ⇒ present, per `:305-309`) and
   `vmm_arch_leaf_is_ro(leaf, &exec_dummy)`; iterate page-aligned from
   `virt_addr & ~(PAGE_SIZE-1)` to
   `(virt_addr + bytes - 1) & ~(PAGE_SIZE-1)`, bounded by
   `bytes <= USER_SPACE_LIMIT` and an overflow guard; `bytes == 0` → false.
3. No per-arch code is added: x86_64 (`vmm_x86_64.cpp:110-115`), aarch64
   (`vmm_aarch64.cpp:129-141`, AP[2] alone) and riscv64
   (`vmm_riscv64.cpp:116-121`, R && !W) already answer the question.

**Phase 2 — shared fault-armed delivery helper (scheduler)**
4. `src/kernel/task/scheduler.hpp`: declare
   `[[nodiscard]] static bool deliver_wait_status(TaskControlBlock &parent,
   uint64_t code) noexcept;` immediately above `wake_waiting_parent`; doc-block:
   performs the parent-CR3 switch, the armed store, the CR3 restore, clears
   `parent.waiting_child_status`, takes no lock, safe from IRQ-masked/ISR
   context, returns false when the store faulted.
5. `src/kernel/task/scheduler_lifecycle.cpp`: implement the helper with this
   exact shape (function-level, not verbatim code):
   - capture `prev_arm = g_user_access_recover_ip`;
   - `bool delivered = false;` before any label-setup;
   - CR3 switch exactly as today (`:405-411`);
   - `g_user_access_recover_ip = reinterpret_cast<uint64_t>(&&recover_status);`
     then `NEXIOS_FAULT_RECOVERY_KEEP(recover_status);`
     (immediately after the arm — no initializer between arm and label);
   - `arch::stac(); *ptr = code; arch::clac(); delivered = true;`
     `g_user_access_recover_ip = prev_arm;` (restore, not `0`);
   - shared tail: CR3 restore, `ptr` cleared to `nullptr`;
   - `recover_status:` label placed **before** the CR3-restore tail with
     `arch::clac();` first, then `g_user_access_recover_ip = prev_arm;` then
     fall through to the shared tail (so the caller's CR3 and the cleared
     status pointer are guaranteed); on that path push exactly one dmesg
     record (Phase 4 code), and return `false`.
6. `src/kernel/task/scheduler_lifecycle.cpp:404-423` (in
   `wake_waiting_parent`): replace the whole inline stac/store/clac/CR3 block
   with `(void)Scheduler::deliver_wait_status(*p, child.exit_code);` — the
   pointer-null test stays the caller's (`if (p->waiting_child_status)`), and
   the clear moves inside the helper (remove `:422`). Rewrite the comment at
   `:412-416` to: mapped **and** writable pre-certified at waitpid entry;
   kernel-task parents are uncertified, hence the armed store; AC cleared on
   both paths.
7. `src/kernel/syscall/syscall_handlers_misc.cpp:162-183`: same substitution
   `Scheduler::deliver_wait_status(*p, t->exit_code)`; delete the inline block
   including its `p->waiting_child_status = nullptr` (`:182`) and fix the
   `:171-176` comment. `waiting_child_pid = 0` (`:184`) stays in place.

**Phase 3 — writability pre-cert + ordering (waitpid entry)**
8. `src/kernel/syscall/syscall_handlers_process.cpp`: move the whole
   publication block so the order is: locate/collect child (`:76-101`) →
   TERMINATED fast path unchanged (`:102-128`) → `WNOHANG` unchanged (`:129-131`)
   → **cert block** → `cur->waiting_child_pid = target_pid;` →
   `cur->waiting_child_status = status_ptr;` → dequeue/BLOCKED (`:146-147`).
   The cert block keeps today's mapped test and adds the writability test:
   for user tasks with a non-null `status_ptr`, require
   `VMM::user_write_window_writable(reinterpret_cast<uint64_t>(status.unsafe_ptr()),
   sizeof(uint64_t), cur->get_page_table())`; on failure return
   `static_cast<uint64_t>(errors::SYS_ERR_FAULT)` (typed EFAULT,
   `syscall_errors.hpp:33`) — apply the same code to the existing mapped-only
   rejection so both certs answer identically.
9. Replace the `:133-138` comment with the two-cert contract (range → mapped →
   writable, whole 8-byte window, fail closed before any wait-state mutation)
   and note that the wake-path arm (Phase 2) is the TOCTOU backstop.

**Phase 4 — observability**
10. `src/kernel/log/dmesg_catalog.hpp`: add
    `{ErrorSubsystem::SCHED, kDmesgBase_SCHED + 20, LogSeverity::WARN,
    "Sched: waitpid status store faulted (not writable)"}` to
    `kDmesgCatalog_SCHED`; before committing, grep for `kDmesgBase_SCHED + 20`
    and `kDmesgBase_SCHED+20` across `src/` to prove the slot is free
    (LEARNINGS: silent code collisions at push time), and mirror the severity
    in `lookup_severity`'s SCHED arm (`:416-420`).
11. Push it exactly once from the recover path in the helper
    (`DmesgService`/`dmesg_push` — SPSC atomics, no lock); nothing is pushed on
    the success path.

**Phase 5 — docs**
12. `src/kernel/task/scheduler.hpp:738-745`: extend the `wake_waiting_parent`
    doc-block — status delivery is armed with the single-slot recovery, stores
    with AC set under the parent's CR3, restores the previous arm, and never
    panics on a bad/RO status page (delivery is skipped, dmesg SCHED+20).
13. `docs/specs/scheduler.md`: if a waitpid/wake section exists, append 3-5
    lines (present ≠ writable; two-cert entry check; armed store at delivery;
    AC cleared on both paths). Verify the section exists first — do not invent
    a new spec file.

**Phase 6 — gates + audit (production change)**
14. `bash ~/jarvis/scripts/healthcheck.sh`; `git branch --show-current` must
    be `main`; `git status --porcelain` recorded in the issue comment (base
    `b843afe78`).
15. Builds: `tools/meson/build.sh` (or the briefing §6 recipe) for all three
    arches, **debug and release**; prove `tools/meson/gen.mk` and
    `src/kernel/meson.build` are untouched (`git diff --stat`).
16. `make check-style` → `Errors: 0`; clang-tidy non-blocking but read the diff
    for new warnings in the touched files.
17. Targeted classes (one at a time, in order, append one `test-history.txt`
    row each with measured time): `process_waitpid` (waitpid contract),
    `scheduler_core` (wake/terminate/watchdog arm), `task_fpu` (
    `test_fpu_context_user` drives a real `SYS_WAITPID`/`wake_waiting_parent`),
    `selftest` (leak/aggregate). Run each on `x86_64` **and** `aarch64` **and**
    `riscv64` (cross-arch parity: the store path is shared but the arm is
    per-arch). No `all`, no `run_all_classes.sh` for a targeted fix.
18. Phase 3 auditor on `git diff main -- <the 7 production files> >
    audits/pending_patch.diff`; on REJECT apply `audits/rejected_patch.diff`
    verbatim (`git apply --check` first) and re-audit. Post the report path +
    `DECISION:` on #588.
19. Post-QE handoff comment with the sketch below; the RO-page tests land on
    `testbed` (QE), never in this change.

## TEST STRATEGY
- New tests: **none authored by dev** (`src/kernel/test/**` read-only). QE
  sketch for `testbed`, one class per concern, stub-first:
  1. `waitpid_ro_status_page_rejected` — parent (user task) passes a status
     pointer into an RO-mapped page (build it with
     `VMM::map_page_ro_in_pml4(va, phys, false, pml4)` — precedent
     `test_pml4_clone.cpp:837`, `test_elf_loader.cpp:1113`); child exits.
     Asserts: `sys_waitpid` returns `SYS_ERR_FAULT`, no panic, child still
     collectible by a retry with a writable pointer, and no
     `waiting_child_pid` residue on the parent (**INV-WAIT-2** pin).
  2. `waitpid_status_window_straddle_rejected` — status pointer 4 bytes before
     a page boundary whose **next** page is unmapped/RO. Asserts: rejected at
     entry (proves the whole 8-byte window is certified, not just the first
     page).
  3. `waitpid_wake_ro_status_recovers` — kernel task waiter (the pre-cert is
     skipped for kernel callers, `syscall_handlers_process.cpp:139`), so set
     `waiting_child_status` to an RO page directly and terminate the child via
     `Scheduler::terminate` (mirrors `test_waitpid.cpp:96-150`). Asserts: no
     panic, dmesg SCHED+20 present, `waiting_child_status == nullptr` after the
     wake, parent woken/READY, and the ResourceTracker delta is 0.
  - Trap to avoid (`test_waitpid.cpp:243`): the existing test maps its status
    word writable with AC set — do not reuse it for the RO cases.
- Existing test classes to run: `process_waitpid` (waitpid/handle contract),
  `scheduler_core` (wake paths: terminate, watchdog, reap), `task_fpu`
  (`test_fpu_context_user` exercises the real waitpid wake end-to-end),
  `selftest` (registry + leak aggregate). Rationale: the change touches
  waitpid entry, the wake store, and the sys_exit duplicate — those three
  classes are the only ones that drive all three paths.
- Cross-arch: every class above on `x86_64`, `aarch64`, `riscv64`; the arm is
  consumed by x86 `kernel.cpp:2533`, aarch64 `interrupt_controller.cpp:509`
  (EL1) / `:341` (EL0) and riscv64 `interrupt_controller.cpp:303` — a green
  x86 run does not prove the off-x86 arms fire.

## SIL 3 / SAFETY RISKS
- [S1] A leaked AC on the recover path (`clac` missing/after a return) —
  debug/x86 panics with "MP-4: AC flag leaked" at the next syscall entry
  (`syscall.cpp:158`); release hands AC to ring 3 through `sysret` (SMAP
  bypass). Mitigation: `arch::clac()` is the first statement of the label,
  both exits clear it, auditor reads the label body line by line.
- [S1] Clobbering an outer recovery arm: the store can run in the timer ISR
  (`scheduler_watchdog.cpp:310`) or a fault handler while an outer guarded
  copy holds the slot; zeroing it would make the outer copy's next fault a
  kernel panic. Mitigation: capture and restore `prev_arm` (equals `0` in the
  common case, so the "cleared on exit" contract and the pager F2 precondition
  still hold).
- [S1] Label placed after the CR3 restore ⇒ the kernel returns on the parent's
  address space. Mitigation: label strictly before the restore, shared tail,
  and the gate in step 17 exercises the wake from `sys_exit`, terminate and
  watchdog contexts.
- [S2] 8-byte store straddling a page boundary: a first-page-only cert passes
  but the second page can be unmapped/RO. Mitigation: `user_write_window_writable`
  certifies every page in `[ptr, ptr+8)`; QE test 2 pins it.
- [S2] TOCTOU between the entry cert and the deferred store (sibling thread in
  the same address space remaps/unmaps while the parent is BLOCKED). A
  pre-cert alone cannot close this — mitigation: the armed store (Phase 2) is
  mandatory, not optional; the cert is the deterministic fail-closed fast path.
- [S2] INV-WAIT-2 violation today: `waiting_child_pid` is published before the
  cert (`:132` vs `:139`), so a rejected pointer leaves stale wait state and a
  later wake re-enqueues a READY parent (ready-queue corruption class).
  Mitigation: step 8 reorders; `audits/plan-586.md:30` already states this
  invariant — the tree simply does not implement it.
- [S2] Reachability is trivial, not theoretical: ELF text is RO-mapped
  (`elf_loader.cpp:593,1589`, `elf.cpp:258`), so any unprivileged task can pass
  a pointer into its own `.text` as `status` and convert a kernel store fault
  into a system-wide DoS. This is why the issue is S2 and why both arms ship.
- [S2] Behaviour change for kernel-task waiters: an armed store converts a
  previously fatal bad kernel pointer into a skipped delivery. Mitigation:
  dmesg SCHED+20 on every recovery so the silent case is observable; QE test 3
  asserts the record.
- [S3] The new VMM predicate must stay walk-only (no `get_table(create=true)`,
  no PMM alloc) or it breaks the lock-free/any-context contract and perturbs
  ResourceTracker accounting; `pte_is_ro_in_pml4`'s test-inspector label must
  not be repurposed as the production API.
- [S3] Both duplicated store sites must be converted in the same commit
  (LEARNINGS: post-edit grep every repeated site) — a single-site fix leaves
  the `sys_exit` path exploitable.
- [S3] The false "present ⇒ cannot fault, no recover_ip needed" comments
  (`scheduler_lifecycle.cpp:412-416`, `syscall_handlers_misc.cpp:171-176`) are
  the mechanism that kept this hazard alive; they must be rewritten, not
  deleted.

## OPEN QUESTIONS
- Ship the armed store (Phase 2) and the writability pre-cert (Phase 3) in one
  commit (planner recommendation: yes — one hazard, two layers), or split so
  the pre-cert lands first and the arm follows as hardening?
- Unify the existing mapped-only rejection onto `SYS_ERR_FAULT` (`:143`, today
  `SYS_ERR_GENERIC_FAIL`) in the same change? It is userspace-ABI visible;
  planner recommendation: yes (no in-tree test asserts the code, libc passes
  the raw value through, `src/libc/unistd.c:44`).
