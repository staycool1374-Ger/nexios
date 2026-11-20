# DESIGN REVIEW — #564 F11 Stage C (carve-composed fork), plan rev 1
Date (UTC): 2026-11-20 · Base verified: `main` = `63199ea16` (git log --oneline -1 confirmed)
Plan: `audits/plan-564-stageC.md` · Binding parents: `plan-564.md` rev 3 + `plan-564-stageB.md` rev 1 + 4 approvals (`design-review-564-r2`, `design-review-564-code-r1`, `design-review-564-stageB-r1`, `design-review-564-stageB-code-r1` — all present in `audits/`)

## VERIFIED (evidence-backed, no finding)

- **Switch publication path**: `scheduler_switch.cpp:750-757` publishes `arch::context_id_tag(next.get_page_table(), publish_context_id_for(next))` into `SwSlots::load_cr3_from`; lazy per-task ID at `:367-371`. TCB-field install is therefore sufficient for the **unpublished composer child** on all 3 arches (x86 CR3+PCID / aa64 TTBR0+ASID via `write_cr3`, `io_impl.hpp:314`; rv64 SATP via `write_cr3`; kernel-half TTBR1 retarget stays inside `switch_address_space`, `:330-333`). No register write and no flush at activation is sound **for a never-live root with a fresh context ID**.
- **VSPACE_ERR_ACTIVE_SPACE gap is real**: `link()` refuses only `vspace_phys == VMM::current_root_phys()` (`vspace_map.cpp:118-119`), and `current_root_phys() = vmm_arch_root_phys(current_pml4())` (`vmm.cpp:593-595`) is the caller's live root only. A registry keyed on activation liveness does not duplicate `VSpaceMap` (structure vs liveness) — the registry direction is the minimal fix.
- **Teardown hazard is real**: `task.cpp:3084-3087` (`free_user_pages` + `context_id_free` + `PMM::free_page`) and `elf.cpp:976-979` (`free_user_pages` + `PMM::free_page`) both assume PMM ownership; `free_user_pages` (`vmm_teardown.cpp:45+`) frees every `PMM::is_user_page`, and user Untyped carves are user-owned — the double-free shape is confirmed. A discriminator + single releaser is the right fix shape.
- **Numbering**: `MAX_SYSCALL = 103`, highest allocated `VSPACE_MAP = 102` (`syscall.hpp:145-146`); 97 = REPLY_RECV (#556 F3), 98–102 consumed contiguously — 103/104 free, no collision. `MAX_SYSCALL` = 105 follows.
- **Spec refs verbatim**: 4.3b sentence at `:274-275`, waiver paragraph at `:679-683`, Appendix A Stage B line at `:992`, §3.4 counts at `:215-220` (KEEP 52 incl. 102; 52+3+2 = 57; 0–104; MAX 105) — all match the plan's citations; N10/N11 rows absent (no collision); §15 row stays `in-progress` in step 16 (no false close).
- **dmesg offsets**: in-tree use is +1,+7,+9,+10,+17–+21 (grepped); +22/+23 free as claimed.
- **Privatization**: `install_page_table` ENSUREs `page_table_ == 0` (`task.cpp:1028-1034`); `swap/clear` fenced per #377. Fork tail (`proc_svc.cpp:93-123`) matches the "shared verbatim" claim.
- **Retrieval structurally present**: 4 graphify queries + 2 shortest-paths (incl. honest graph-absent disposition for `CarvedObjectCap`/`VSpaceMap` nodes + `graphify update` gate 17) + 3 vault searches (0-match recorded) + prior-audit hits (558-code-r1 TLB handoff `:20`, dispose ordering `:15`, lock precedent `:9`).
- **False-close**: composer behind `CONFIG_PROC_MGR_CARVE_FORK` default 0, `clone` retained as comparator + release default, waiver LIFT reworded (not deleted), §15 in-progress. No false-close.

## FINDINGS

- [S1] `src/kernel/syscall/syscall_handlers_vspace.cpp` (new `sys_vspace_activate`, plan step 8) + INV-CARVE-6 overreach — bare `VSPACE_ACTIVATE` (104) on a published task is unsafe
  WHY: (a) SMP TOCTOU: `state != RUNNING` / `page_table_ == 0 || == root` checked with no `scheduler_lock_` exclusion, yet `scheduler_smp.cpp`/`CONFIG_MAX_CPUS` confirm the target can be dispatched on another CPU between gate and `install_carved_root` — a root swap under a live task with no shootdown, contradicting INV-CARVE-6's own "the root was never live" premise (true only for the unpublished composer child); (b) no kernel-half `merge`/`assert_kernel_half_converged` on the bare path (composer step 5 is not replicated; `validate_subtree` refuses kernel-half *components* but never requires convergence) — first dispatch on a kernel-half-less root faults on kernel entry; (c) no `is_user_` gate — the `page_table_ == 0` arm admits kernel tasks; (d) `pcid_` overwritten by a fresh `context_id_alloc()` without freeing the incumbent, bypassing the stale-tag discipline (`task.cpp:3078-3083`) and leaking the ID. The mechanism under-implements INV-VS-3-prime(b).
- [S2] `sys_vspace_frame_map` (plan step 7) — no `ACTIVATED`-root refusal, inconsistent with step 5a for `VSPACE_MAP`
  WHY: post-activation frame insertion escapes the activation-time `validate_subtree` guarantee, and the CLONE-kind interaction is unanalyzed (carved VSpace mapped via 103 then bound to a non-carved task takes the CLONE teardown arm, where `free_user_pages` frees FrameCap-owned leaves — the S1 double-free shape of finding-class 3).
- [S2] `carve_binding.hpp` (new, plan step 3) — `s_active_roots_[CONFIG_CAP_MAX_VSPACE_MAPPINGS]` index function unspecified
  WHY: the lock-free probe `carve_root_is_activated(phys)` needs a phys→slot map that is never defined (hash? slot-index scan?); lock-freedom, collision behaviour, and phys-reuse ABA are unverifiable as specified.
- [S2] `task.cpp`/`elf.cpp` (plan steps 12–14) — exec-path accounting incomplete for carved tasks
  WHY: the CARVED `release_address_space()` arm omits `VSpaceMap` link invalidation, so every exec of a carved task leaks link-registry slots until fail-closed `REGISTRY_FULL`; and the exec-installed PMM root keeps `VspaceKind::CARVED`, so the next teardown takes the drain-only arm and never frees the PMM root (leak per exec, tracker-visible but unmitigated).
- [S2] plan steps 10.4/10.9 vs §NOT-touched + INV-CARVE-10 — internal contradiction on `clone` identity
  WHY: extracting the register-frame/FPU blocks into shared helpers edits `clone`, but the plan simultaneously requires `clone` to stay "statement-identical" as the knob-OFF comparator and claims OFF-build identity — the comparator's integrity cannot be verified if its text changes.
- [S3] plan step 10.137 + INV-CARVE-2 — rollback phrasing and error mapping imprecise
  WHY: "every failure = `destroy(child)`" is undefined before step-4 adoption (child may not exist; guard-only path should be stated), and the "must never return `VMM_ERR_PAGE_ALLOC`" assertion has no pinned call-site mapping (debug ENSURE or typed map) at the `ForeignFrameMap::map`/composer layer.
- [S3] plan gate 17 — two-knob matrix not enumerated
  WHY: `CONFIG_PROC_MGR` × `CONFIG_PROC_MGR_CARVE_FORK` = 4 combos; "knob ON and OFF" is ambiguous about which knob(s), and the PD-knob-ON × carve-knob-ON fork path (verdict leg + composer) is not named as a gate leg.
- [S3] plan §Retrieval — citation precision
  WHY: `design-review-564-r1.md:42` does not verbatim contain the quoted 4.3b sentence (content is paraphrased from the 564-r1 scope discussion); re-verify line refs before the audit record is frozen.

## COVERAGE HONESTY (prompt check 8)

Claimed 11 invariants (INV-VS-3′ + INV-CARVE-1..10) and 8 S1 risks — counts match the text (8 S1 + 4 S2 + 4 S3 = 16 risks). Coverage is not fully honest: the bare-activation live-target hazard class (S1 above: TOCTOU swap, missing kernel-half merge, missing `is_user_` gate, pcid leak) has no corresponding risk entry — the plan's S1 set covers only the composer path, where activation is safe. Generation-on-carved-child, hold-on-carved-child, cross-arch W^X (#376), and HHDM fixture rule coverage are adequate.

## PATCH

`audits/rejected_patch.diff` not written: this is a design-plan review (no code diff submitted); corrections travel as the numbered findings above for the plan rev 2 edit.

DECISION: REJECTED
