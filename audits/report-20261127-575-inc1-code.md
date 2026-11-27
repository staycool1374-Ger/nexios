# AUDIT REPORT 20261127-575-inc1-code
PATCH: audits/pending_patch.diff
FILES: docs/specs/oom-rt.md, src/kernel/ipc/buffer_pool.cpp, src/kernel/memory/mem_pressure.cpp, src/kernel/memory/mem_pressure.hpp, src/kernel/memory/mempool.cpp, src/kernel/memory/pmm.cpp, src/kernel/memory/untyped_handover.cpp

## VERIFICATION (static, tree-checked 2026-11-27)
- Scope is exactly Increment 1 steps 1-8: mem_pressure hpp/cpp (latch_ordinal/latch_notable/take_pending/latch_tick/snapshot_reset, drain_pending deletion, MEMPOOL_EMPTY=6 + PMM_PRESSURE=7); pmm.cpp (note_budget_deny funnel, OOM latch, note_frozen_deny row budget); mempool.cpp (2 MEMPOOL_EMPTY latches); buffer_pool.cpp + untyped_handover.cpp (row budgeting only); oom-rt.md new §6. No scheduler_tick, no notify, no syscall, no test-tree files touched.
- INV-1 latch body allowlist holds: latch_ordinal is relaxed fetch-add on g_count + first-wins CAS on g_latched + winner-only relaxed stores (reason/pending/g_latch_tick) + arch::Timer::ticks() + log::dmesg_push_sev (mem_pressure.cpp:21-45). dmesg_push_sev -> DmesgService::push -> DmesgBuffer::push is SPSC atomics + Timer::ticks + current_task read + bounded copy, no locks, no allocation (dmesg.hpp:260-274, 356-398). Timer::ticks declared per-arch (timer.hpp:76,165,233). Safe from inside pmm_lock_/mempool_lock_/any IRQ.
- INV-2 no poke added: diff contains no Scheduler::, no set_task_ready, no Notify, no service_pending, no scheduler_tick edit. Notify path untouched; poke arrives in Increment 2 as planned.
- INV-3 first-wins + ordinal + 1-per-64: ordinal = fetch-add + 1 before CAS so every caller counts (:23-24); losers return ordinal without touching shared state (:28-31); latch() is thin wrapper, latch_notable = ordinal % 64 == 0 (:47-53); winner emits exactly one PMM+4 row (:40-43). Row budget converts 4 pre-existing unconditional rows (PMM+2 frozen, BUFPOOL+1 x2, handover PMM+4) to latch_notable-gated with unchanged deny returns.
- INV-4 production never clears: clear()/snapshot_reset() have zero production callers (grep: only test_mem_handover.cpp and test_microkernel_transition.cpp call clear(); take_pending/snapshot_reset have zero callers — Increment 2 seam). drain_pending deleted with zero remaining references (grep clean). No syscall path clears.
- INV-6 not yet wired: PMM_PRESSURE enum added but no latch site, no baseline/waterline/service_pending logic in diff — correctly deferred to Increment 2.
- INV-7 zero-alloc at all new sites: note_budget_deny (pmm.cpp:40-42) and both mempool latches sit on deny legs returning 0/nullptr/MEMPOOL_ERR_OOM under held locks; latch + dmesg add no allocation-capable call. Frozen/buffer/handover deltas add no calls on the deny path itself, only gate the pre-existing row.
- INV-8 arch-neutral: no arch conditionals, no CONFIG_ARCH, no per-arch files in diff.
- Budget funnel single-site: is_over_budget() true-legs are the only BUDGET_DENY insertion (pmm.cpp:443-454, kept under #if CONFIG_MEMORY_BUDGET with #else false). All in-tree callers deny on true (return 0 / typed OOM): alloc_page :497, alloc_contiguous :731, and the 9 further sites matching the 11 lines verified at plan audit. No advisory-only caller.
- OOM latch after unlock with guard: run_oom_handler_locked early-returns {false,false} on !oom_handler_ (:394-396, unlatch ed — no policy configured is not exhaustion); latch on !retry only, while pmm_lock_ is released (:397-405), then re-locks (:406). Covers all OOM/_err legs uniformly.
- Too-large leg unlatched by design: mempool alloc !has_pool -> nullptr (:101-102) and alloc_err !has_pool -> MEMPOOL_ERR_TOO_LARGE (:395-397) carry no latch; only free_count==0 legs latch (:105-111, :400-403, mempool_lock_ held — allowed by INV-1).
- Frozen deny still fail-closed: note_frozen_deny only gates the PMM+2 row (:53-58); every caller keeps unconditional ASSERT + return 0 (e.g. :485-489, :719-725). Deny never conditional on the row budget.
- Buffer/untyped delta exactly the row budget: same latch reason, same dmesg code/message/context, same typed return (BUF_ERR_OOM; g_result = err); only wrapped in if (latch_notable(...)).
- No new diagnostic codes: PMM+4 (winner + handover), PMM+2 (frozen), BUFPOOL+1 all pre-exist in dmesg_catalog.hpp (:261-274). Diff reuses them.
- SHALL text untouched: diff adds oom-rt.md §6 only; no SHALL lines exist or are modified in that file; no spec normative statements altered.
- Preprocessor symmetry: note_frozen_deny defined and called only under CONFIG_STATIC_POOLS_ONLY; is_over_budget latch legs inside CONFIG_MEMORY_BUDGET true-branches with #else returning false — no asymmetric/uninitialized path.

## GATES (reported evidence, corroborated in test-history.txt 2026-11-27)
- x86 debug rows present: mem_handover 7/7, memory_resource_exhaustion 5/5, core_memory 136/136, core_sched 135/135, scheduler_preemption 13/13, core_sync 105/105, capability 200/200, bench_microkernel 12/12, selftest 165/165 (10:41-10:44 block). aa64/rv64 rows present: mem_handover 7, resource_exhaustion 5, core_memory 132, core_sched 135 (10:44-10:46 block). Style Errors 0 and 3-arch build+link claimed. Auditor did not re-execute the harness (per role); verdict rests on static review + reported gate rows.

## DEVIATIONS (disclosed, accepted)
- pools1 3-arch link BLOCKED by pre-existing QE-owned test rot (test_mem_handover.cpp:543 g_flag -Werror, identical at parent a004f9753, reported on #573). Accepted: pending_patch.diff touches no test files; test-file fix is QE-owned and out of production-audit scope.

## FINDINGS
- none. All code-audit checks pass as specified.

DECISION: APPROVED
