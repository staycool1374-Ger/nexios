# DESIGN AUDIT REPORT — F9 static bound re-sizing, issue #562 (plan-562)

PLAN: audits/plan-562.md (base main @ 5304d8883)
SCOPE: design review only — no code diff exists yet. Normative sources read at
the plan's pinned base: docs/specs/microkernel-transition.md (§7.2, §7.3,
§11, §15 F9/F21 rows, §16 Q7), src/kernel/nexios_config.h, consumer sites
(iopb.cpp, iommu.hpp/cpp, derivation.hpp, task.cpp, boot_sdf.cpp, pmm.cpp,
mempool.cpp), regression probes (test_cap_mmio.cpp, test_cap_iommu.cpp),
riscv64 boot.S, docs/specs/configuration.md, docs/specs/iommu.md.

## FINDINGS

- [S3] microkernel-transition.md:911 (§15 F9 row) — F9 title omits IOMMU while §16 Q7 (:935) routes "IOMMU 8 domains / 128 mappings per domain" to "landed via F9/#562"; no other follow-up (F1/F4/F5/M4/F21) owns IOMMU bounds.
  WHY: The plan's Q-F9-1 default (land 128 in F9) is the only reading that leaves no decided value unlanded; owner to confirm on the thread before merge (merge condition, not a plan defect).
- [S3] microkernel-transition.md:459-464 + :788 (§7.3/§11 normative edits) — #582 precedent requires owner sign-off for normative rows, but Q7 (:935) already IS the recorded owner decision for these exact values.
  WHY: The edits are mechanical bookkeeping to Q7, not new decisions; owner ack of Q-F9-2 on the thread suffices, no separate sign-off round needed.
- [S3] src/kernel/arch/x86_64/hal/iopb.cpp:45 — the `uint32_t g_iopb_claimed` mask shifted by `1u << slot` (:74, :80, :82, :96) is the true ceiling (32), not the count macro; the plan documents this (INV-B2, S2 note) but ships no tripwire.
  WHY: A future raise past 32 would be silent UB; recommend `static_assert(CONFIG_IOPB_MAX_TASKS <= 32)` in iopb.cpp as follow-up hardening (plan's F21-class punt is acceptable for this values-only patch).
- [S3] docs/specs/iommu.md:40 — tunable row also cites `CONFIG_CAP_MAX_IOMMU`(16); verified present in tree (nexios_config.h:850-851), not a Q7 row, untouched by the plan.
  WHY: Noted only to record it was checked and correctly excluded from the residual.

## VERIFIED (no finding — load-bearing checks closed with evidence)

- S1 riscv64 fail-stop: boot.S:40-58 fail-stops when `kernel_phys_end >= _boot_stack_rv − 2MiB` (stack at 0x82000000 → bound 0x81E00000). #510 end 0x80C77000 → ~17.3 MiB headroom; worst-case growth is +18 KiB on riscv64 (+50 KiB x86_64 = 32 KiB IOPB + 18 KiB IOMMU; IOPB pool is an x86-only TU). Margin ~350-980x. x86_64/aarch64 boot stacks are linker sections with no image-size fail-stop. INV-B6 mitigation (riscv64 boot + record `kernel_phys_end`) is the correct residual action.
- S1 IOPB 8: `1u << slot` over `uint32_t` safe at 8 (ceiling 32); pool 8×8192 B = 64 KiB .bss; lock/RT-path unchanged.
- S1 IOMMU 128: `IoMmuMapping` = 8+8+4+1(+pad) = 24 B; 8 domains × 128 × 24 B = 24 KiB (+18 KiB). All table scans bounded by the macro (iommu.cpp:573,616,628,675,799); `iotlb_invalidate`/`ctx_cache_invalidate` are count-independent; domain-count paths untouched.
- S2 residual EXACT: Q7's 12 value items partition cleanly — landed (UNTYPED 64 @ :591, CSL descriptionspot-checked; DEATH_WATCHES 32 @ :751; CSLOT 64 @ :575; DOMAINS 8 @ :858; MEMPOOL removal @ :293-300; TCB assert task.hpp:1474; unchanged 64/48/32/256 @ :58/:228/:982/:123), values-residual here (IOPB 4 @ :704, MAPPINGS 32 @ :864), structural → M4/M11 (per-row MAX_PER_TASK_OBJECTS: attach_object task.cpp:280-290 enforces no budget, detach walk-bound :319-338 — a flat 32→64 raise would widen the walk without adding control, so no interim raise is owed; 2nd-CNode: sole production caller task.cpp:269), F21 #573 (alloc_page_table :949-979 pool fast path has no frozen check — fix needs pmm.cpp code edit, and §15 F21 row :923 names that exact entry point), SDF demand-vs-bound → M4 (sdf_compose boot_sdf.cpp:670 checks kMaxCapsPerRole/policy/zero-cap but no bound-demand validation).
- S2 doc-sync deltas accurate: §7.3 four stale cells (UNTYPED 16, DEATH 16, IOMMU 8/32, IOPB 4) → Q7 values; §11 :788; configuration.md :136/:139 (prose carries no hardcoded counts); iommu.md :40.
- S2 consumer-assert matrix complete: 256 >= 4×64 exact at derivation.hpp:70 (do-not-touch-CSLOT correctly called out); paired `#error` defaults (:1701, :1734) both updated in-plan (INV-B1).
- S3 probes self-adapt: ioport test claims MAX+1 = 9 TCBs against 64-block 8 KiB MemPool class (mempool.cpp:48-50) — fits; iommu test allocates 129 pages + 129 FrameCaps (~64 B class, 320 blocks) + 2×129-entry stack arrays (2064 B < 64 KiB CONFIG_STACK_SIZE :177) — fits; both iterate the macros, no test edits needed; count rows unchanged.
- S3 check-7 retrieval present (Appendix C: 3 graphify + 5 vault queries with dispositions, negative-result and reachability control included).
- S3 scope clean: 4 config lines + 2 paired `#error` messages in 4 separated hunks (Appendix E line-accurate against base), docs-only otherwise, M4 handoff note (Appendix D) concrete with file:lines and mechanism sketch. M4-rebase isolation adequate.

DECISION: APPROVED
