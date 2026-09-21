# AUDIT REPORT 20260920T190825Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/test/test_cross_arch.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_kernel_isolation.cpp

## FINDINGS
- [S2] src/kernel/test/test_cross_arch.cpp:902 — TEMP-DBG probe remnant left in committed test code
  WHY: Logger::error("TEARDOWN-PD64...") fires on the documented normal path (higher PD[0] split predates all tests), violating the zero-probe-remnant criterion and injecting per-run error-level serial noise into timing-sensitive tests.
- [S3] src/kernel/test/test_cross_arch.cpp:899 — absent-or-tracked shape asserts codify unidentified occupants (slot 257, PD[125], user-buffer windows) as pass
  WHY: Accepted as principled (PMM-tracked invariant + round-trip entry-equality + ResourceTracker deltas backstop it), but occupant identification stays open on #199 and a tracked-but-unintended page would pass this check alone.
- [S3] src/kernel/test/test_cross_arch.cpp:1054 — #else clone-dispose pattern (clone/free_user_pages/free_page) has no in-tree precedent and cross-arch compile is locally unverified (disclosed)
  WHY: Balanced by inspection (single top-page alloc/free, empty user half, arch-neutral VMM/PMM APIs only), test-only, no production impact.
- [S3] check 7 — issue #199 existence/title/labels/milestone verified via fetch, but comments are not renderable without auth so graphify/vault artifacts are unverifiable from this environment
  WHY: Inconclusive fetch is not proof of absence; relying on invoker assertion (BFS-d3 query pasted, obsidian 0 hits, planner deviations documented).

## VERIFIED CLEAN (no finding)
- Checks 1/5: TEST-ONLY patch, zero production files touched; shape/identity tests are read-only live walks; tlb_flush covers every manual restore (roundtrip x2, priv-teardown x1 each).
- Check 2: IrqGuard scope correct everywhere; JARVIS_ASSERT is record+return (src/lib/test.hpp:300), so stack unwinding runs the guard dtor — no abort-while-holding-guard hazard.
- Check 4: split PT comes from PMM::alloc_page_table (vmm.cpp:317) and PMM::free_page is pool-aware + bitmap-guarded idempotent (pmm.cpp:708-735); free_priv_window early-return chain verified (test_kernel_isolation.cpp:46-82); new ki tests mirror the existing balanced precedent (:158-166).
- Check 6: aarch64 has pml4_index (aarch64/hal/page_table_impl.hpp:99), riscv lacks it and all riscv-falling #else branches avoid it; counts arithmetic exact (21+4=25, 4+2=6, 110+7+4=121); core 436-vs-462 drift pre-existing, out of scope.
- Direct __atomic_store_n to VMM::identity_modified_ is acceptable: public documented member (vmm.hpp:364-375) with __atomic_* contract, no set_true API exists, test-only, clean at exit.

## PATCH
audits/rejected_patch.diff was written (git apply --check passes) and removes the TEMP-DBG comment + Logger::error probe block, keeping the strict huge-or-tracked-PT assert.

DECISION: REJECTED
