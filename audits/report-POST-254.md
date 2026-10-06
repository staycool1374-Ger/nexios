# AUDIT REPORT POST-254
PATCH: audits/pending_patch.diff
FILES: src/kernel/memory/address.hpp, src/kernel/memory/checked_ptr.hpp, src/kernel/memory/integrity.cpp, src/kernel/memory/integrity.hpp, src/kernel/memory/mempool.cpp, src/kernel/memory/mempool.hpp, src/kernel/memory/mempool_errors.hpp, src/kernel/memory/pmm.cpp, src/kernel/memory/pmm.hpp, src/kernel/memory/pmm_errors.hpp, src/kernel/memory/tlb_shootdown.cpp, src/kernel/memory/vmm.cpp, src/kernel/memory/vmm.hpp, src/kernel/memory/vmm_errors.hpp
## FINDINGS
- [S3] vmm.cpp:1290 — warn_test_kernel_touch evaluates gate before Scheduler::is_test_active(), swapped vs original (is_test_active() && gate) order
  WHY: Both conjuncts are pure and side-effect-free so the observable gating is identical, but the short-circuit order is technically not bit-identical.
- [S3] pmm.cpp:825 — alloc_page/alloc_contiguous/alloc_page_colored re-read Scheduler::current_task() when OomOutcome.handler_ran, where the original kept the pre-handler cur for attribution
  WHY: Re-reading after unlock→handler→relock attributes budget/accounting to the actually-current task and cannot corrupt bitmap/owner state, so it is safe but not literally zero-delta.
DECISION: APPROVED