# AUDIT REPORT PRE-254
PATCH: (none — pre-implementation plan review)
FILES: src/kernel/memory/pmm.cpp, src/kernel/memory/vmm.cpp, src/kernel/memory/mempool.cpp, src/kernel/memory/checked_ptr.hpp, src/kernel/memory/address.hpp, src/kernel/memory/integrity.cpp, src/kernel/memory/tlb_shootdown.cpp
## FINDINGS
- [S3] step-1 vmm.cpp:277,334,346,449,470,476,535,605,644,648,764 — plan says 10x is_test_active() sites, source shows 11 matches; helper note_test_kernel_va_touch() must cover all sites with per-site predicates preserved bit-identical
  WHY: A missed or over-merged site would silently change test-vs-production VA-touch behavior.
- [S3] step-3 pmm.cpp:416-428,483-495,519-529,570-581,606-616,633-643,660-670,689-699,972-982,1005-1015,1033-1043,1066-1076 — single oom_retry_locked helper must preserve per-callsite error-code discipline (PMM_ERR_OOM vs USER_OOM vs TABLE_OOM) and return-type convention (0-phys vs error-code) plus unlock→handler→relock order
  WHY: Collapsing divergent epilogues into one helper risks mapping the wrong error code or reordering the lock boundary.
- [S3] step-2/step-4 alloc_from_window_locked(user_owned)/alloc_colored_locked/pop_block_locked/push_block_locked/find_owner_pool — shared helpers with ownership/pinned parameters must preserve kernel/user accounting and pinned-block divergence (free warns+returns mempool.cpp:142-149 vs free_err silent mempool.cpp:437)
  WHY: A wrong user_owned flag or unified pinned path would cause S1 accounting drift or change free-vs-free_err contract.
- [S3] step-8 note_silent_drop_once() — new shared throttle on previously silent-drop returns must be diagnostics-only, severity-gated, bounded, and never called from ISR or lock-held fast paths (precedent: vmm.cpp:1168 log_skip; tick path uses IrqGuard+try_lock-skip tlb_shootdown.cpp:130-131)
  WHY: Logging from ISR or under pmm_lock_/mempool_lock_/shootdown_lock_ risks deadlock or recursion on RT paths.
- [S3] step-5/step-6 kNoPool/has_pool()/kMagic*/kFupSkipLogCap=24/named predicates — literal-for-constexpr swaps must be value-identical and predicate extraction order-preserving; verify 24 matches existing cap and each kMagic equals its integrity.cpp:56-83 literal
  WHY: A mistyped constant or reordered predicate changes OOM/sentinel/budget decisions while looking like pure hygiene.
DECISION: APPROVED
