# AUDIT REPORT 2026-09-20T23-13-04Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/task/scheduler.cpp, src/kernel/task/scheduler.hpp, src/kernel/test/test_smp_sched.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_isolate.cpp, src/kernel/memory/pmm.cpp, src/kernel/memory/pmm.hpp, src/kernel/memory/vmm.cpp

## FINDINGS
- [S3] src/kernel/memory/pmm.cpp:28 — overlay bit index `idx - page_table_pool_start_ / PAGE_SIZE` relies on /-over-- precedence and has no `bit < CONFIG_PAGE_TABLE_POOL_SIZE` clamp; pool size is fixed to CONFIG at both init paths so in-bounds by construction, but add parens + clamp for defense-in-depth (overlay is 512B BSS; a corrupt pool_free_head_ would OOB-write where pre-existing code only mis-set the large bitmap).
  WHY: Correct value today, fragile expression with a new small-array OOB primitive on an already-untrusted index.
- [S3] src/kernel/task/scheduler.cpp:161 — quiesce_exit does load-then-sub_fetch (TOCTOU under truly concurrent exits); all current call sites are serialized (scheduler_lock_ for scheduler.cpp sites, BSP+IrqGuard for harness/tests), so unreachable today; prefer checking the sub_fetch result alone.
  WHY: Depth underflow to UINT64_MAX would wedge the AP parked; excluded by existing serialization, not by the code shape.
- [S3] src/kernel/memory/vmm.cpp:117,137 — new modified-flags live in the non-RISCV branch only, so RISCV map_page_in_pml4 splits still skip the PD rewind; mirrors the pre-existing map_page asymmetry and the affected tests are x86_64-only, but the gap should be a tracked note.
  WHY: Cross-arch conditional asymmetry of the same class the audit checklist targets; no x86 behavior risk.
- [S3] src/kernel/memory/pmm.cpp:58 — overlay-apply can re-mark a pool page whose PD-split parent was detached by the PD rewind in the same restore (bounded single-PT-page leak, fail-conservative direction); ResourceTracker + exact-count gates would surface it, and no silent corruption results.
  WHY: Leak-vs-untrack tradeoff favors safety; worst case is a counted leak, not UAF or live-table pruning.

## CHECKS 1-7 DISPOSITION
1. Allocations: none — static 512B overlay, stack-only RAII guards. PASS.
2. Concurrency: overlay set/clear/apply/clear all under pmm_lock_; quiesce sites paired on all paths (set_affinity_err 3 exits, balancer_tick 2 exits, cleanup 1, create no-return span 424-656, restore no-return span 1098-1461 verified); test windows hold IrqGuard+quiesce with no polls/sleeps inside. PASS.
3. Assertion masking: drain-test asserts got STRICTER (==0 pre, ==1 post, pre-drain solitude); no timing assert weakened. PASS.
4. Memory safety: free_page clears overlay before pool-freelist return on every pool path; double-free no-ops via bitmap_test; restore-refusal still applies conservatively. No double-free/UAF introduced. PASS.
5. Critical-section interference: extended window covers only BSP-local restores (BufferPool/ResourceCounters/kstack/TSS/iopb/mmio/deathnotify/pager/msix/rebuild_ready_queue); no AP-progress waits; reload_daemon_tasks nesting composes via the depth counter (required pairing, delivered together). PASS.
6. Preprocessor/conditionals: no new #ifdef; is_test_active gates all VMM production-path changes (zero non-test behavior delta); RISC-V asymmetry pre-existing. PASS.
7. Retrieval: input records graphify BFS-d3 MemPool-pin query (used), vault 0 hits, cycle-1 report + planner claim-by-claim verification. PASS.
TEMP sweep: zero hits for TEMP-DBG/TEMP-DIAG/TEMP-BISECT/MB2DBG/CMDBG/FDTDBG/DIAG-FAULT/H2_RING_ONLY/QEMU_INT_LOG in the patch. Nesting test asserts only after window close (no AP wedge on failure). PASS.

DECISION: APPROVED
