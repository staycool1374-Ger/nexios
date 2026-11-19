# AUDIT REPORT 2026-11-19T00:00:00Z (design review, issue #588 r1)
PATCH: none (design review of audits/plan-588.md; no code diff submitted)
FILES: src/kernel/syscall/syscall_handlers_process.cpp, src/kernel/task/scheduler_lifecycle.cpp, src/kernel/syscall/syscall_handlers_misc.cpp, src/kernel/task/scheduler.hpp, src/kernel/memory/vmm.hpp, src/kernel/memory/vmm_map.cpp, src/kernel/log/dmesg_catalog.hpp, docs/specs/scheduler.md

## PLAN CLAIMS vs TREE TRUTH (all 7 verified)

1. TWO unarmed stac stores, same false rationale — CONFIRMED.
   scheduler_lifecycle.cpp:417-419 (`arch::stac(); *p->waiting_child_status = child.exit_code; arch::clac();`)
   under parent-CR3 switch (:405-411) + restore (:420-421), with the false
   comment at :412-416 ("present ... no recover_ip is needed").
   Identical shape + identical false comment in sys_exit,
   syscall_handlers_misc.cpp:177-179 with comment at :171-176, CR3 switch
   :163-170, restore :180-181, clear :182. Second site genuinely stores to a
   user pointer under stac with no arm.
2. INV-WAIT-2 violation — CONFIRMED, and it matters.
   syscall_handlers_process.cpp:132 publishes `cur->waiting_child_pid` BEFORE
   the mapped-only cert at :139-144 and the status-pointer store at :145.
   On cert rejection (:143 `return GENERIC_FAIL`) the pid stays published on a
   still-READY task (dequeue/BLOCKED at :146-147 never runs); a later
   `wake_waiting_parent` matches the stale pid and re-`set_task_ready`s an
   already-ready task (intrusive ready-queue corruption class). Reorder is required.
3. Recovery primitive exists on all three arches and fits — CONFIRMED.
   `g_user_access_recover_ip` declared at src/kernel/memory/checked_ptr.hpp:34;
   `NEXIOS_FAULT_RECOVERY_KEEP` at :52-57 (same-frame + no-init-crossing rules
   at :47-51). Consumed: x86 kernel.cpp:2533-2536 (vector-14 gate),
   aarch64 interrupt_controller.cpp:341-345 (EL0) and :509-513 (EL1),
   riscv64 interrupt_controller.cpp:303-306. An 8-byte direct dereference under
   stac is exactly the shape every checked_ptr/safe_copy site already arms;
   all three fault paths redirect RIP/SEPC/ELR to the label. Fits.
4. The three S1s — all CONFIRMED against the tree.
   (a) AC leak: debug panic is literally `panic("MP-4: AC flag leaked into
   syscall entry (missing clac)")` at syscall.cpp:158, inside the :150-159
   detector whose comment states sysret hands AC to ring 3 (FMASK masks IF
   only). `clac`-first label is mandatory. (b) Save/restore (not zero):
   every in-tree consumer zeroes the slot on consume (kernel.cpp:2535,
   aa64 :343/:511, riscv :305), so a nested arm today clobbers an outer one;
   the nesting path is real — timer-tick preemption of an outer guarded copy
   (kernel.cpp:2527-2532 documents ticks landing inside guarded copies, issue
   #252) → watchdog KILL arm → wake_waiting_parent at
   scheduler_watchdog.cpp:310 while the outer arm is live; and the aa64 EL0
   fault handler wakes waitpid parents from IRQ-masked context
   (interrupt_controller.cpp:318-321). Save/restore of `prev_arm` is the
   correct deviation (common case prev==0 preserves the cleared-on-exit /
   pager-F2 contract, external-pager.md:366-374). (c) Label-before-CR3-restore:
   switch/restore ordering confirmed at scheduler_lifecycle.cpp:405-411 /
   :420-421 (misc :163-170 / :180-181); a label after the restore would resume
   the kernel on the parent's address space. Shared tail is correct — and it
   also keeps the dmesg push on the kernel-half-converged mapping.
5. S2 reachability via ELF .text — CONFIRMED, trivial.
   Non-writable segments map RO via `map_page_ro_in_pml4` (elf_loader.cpp:
   1588-1590, `!(flags & PF_W)` → RO with exec per PF_X; golden path :593),
   so .text is a present-but-RO user page in the task's own pml4. `checked()`
   validates range only, and `virt_to_phys_in_pml4` returns nonzero for a
   present RO leaf, so the current mapped-only cert passes it and the deferred
   stac store faults (SMAP/permission #PF) with no recovery — kernel panic /
   watchdog cascade from an unprivileged pointer choice.
6. Design soundness — SOUND on both layers; (B) is NECESSARY, not optional.
   (A) `user_write_window_writable`: `pte_leaf_in_pml4` non-null implies
   present (:305-309) and excludes huge/block leaves; per-arch
   `vmm_arch_leaf_is_ro` exists in all three TUs (vmm_x86_64.cpp:110,
   vmm_aarch64.cpp:129, vmm_riscv64.cpp:116); page-aligned whole-window walk
   with overflow guard, `bytes==0 → false`, fail-closed. Sound.
   (B) Shared `deliver_wait_status`: lock contexts verified —
   `scheduler_terminate.cpp:107-135` calls `wake_waiting_parent` under
   `SchedulerLockGuard` (same scope), watchdog KILL at
   scheduler_watchdog.cpp:307-311 is lock-free by design (:347-353), aa64 fault
   context is IRQ-masked with wake explicitly allowed (scheduler.hpp:738-744).
   Helper takes no lock, allocates nothing, logs only via lock-free SPSC
   dmesg (dmesg.hpp:260-273, precedent: watchdog already logs from tick
   context). Safe in all three contexts. TOCTOU verdict: (B) is REQUIRED
   because (i) kernel-task waiters skip the pre-cert entirely
   (syscall_handlers_process.cpp:139 `syscall_is_user_task()` gate) so (A)
   covers them zero percent; (ii) the PTE walk itself races concurrent table
   mutation by sibling threads; (iii) the wake path (child-death only) is a
   cold path, so the "machinery in a hot path" objection does not apply.
7. Scope/parity/test-sanctity — SATISFIED.
   No test-file writes (QE sketch only); sketch is implementable as specified
   (ro_page_rejected / window_straddle_rejected / wake_ro_status_recovers with
   assertions incl. INV-WAIT-2 pin, ResourceTracker-0, dmesg SCHED+20), and
   every precedent cited exists: `map_page_ro_in_pml4` test use at
   test_pml4_clone.cpp:837 and test_elf_loader.cpp:1113, terminate-mirror at
   test_waitpid.cpp:96-150, writable-status trap at test_waitpid.cpp:243-247.
   DMESG slot +20 verified free (catalog +16..+19 at dmesg_catalog.hpp:281-288,
   no +20 anywhere in src/; severity mirror arm at :416-429 ready to extend).
   `SYS_ERR_FAULT` exists (syscall_errors.hpp:33). Cross-arch: predicate is
   arch-generic over per-arch leaf decoders; arm consumed on all three arches;
   step 17 gates every class on x86_64+aarch64+riscv64. Check-7: step 0 pastes
   4 retrievals with dispositions (2 used, 1 negative-path documented, zero-hit
   vault searches marked ignored with reason) — meets the design-stage
   evidentiary bar. Spot-checked 12+ anchors; worst drift is a few lines
   (comment/record ranges), none material.

## FINDINGS
- [S3] audits/plan-588.md:309-311 (Open Q2) — unifying the existing mapped-only rejection (:143, today SYS_ERR_GENERIC_FAIL) onto SYS_ERR_FAULT is userspace-ABI visible (libc passes the raw value through, src/libc/unistd.c:44-46).
  WHY: A design that silently changes a veteran error code needs an explicit ABI note in the issue before implementation; recommend adopting FAULT for both rejections (both are fault rejections) with the delta recorded.
- [S3] audits/plan-588.md:305-307 (Open Q1) — ship both layers + both sites in ONE commit.
  WHY: A split landing (pre-cert first, arm later) leaves the sys_exit path and all kernel-task waiters exploitable in between; the plan already states single-site fixes are insufficient (S3 note in §SIL 3/S3), so the commit plan should not re-open it.
- [S3] src/kernel/syscall/syscall_handlers_process.cpp:76-132 (pre-existing, out of scope) — child terminating between the locate scan (:76-99) and pid publication (:132) misses its wake (pid unpublished when wake runs) and the parent BLOCKED-waits forever.
  WHY: The reorder in step 8 neither widens nor closes this window, so it is not a #588 blocker, but it is the same wait-state class of bug in the touched function — file a follow-up issue (re-scan after publish / publish under lock) rather than expanding this plan.

DECISION: APPROVED
