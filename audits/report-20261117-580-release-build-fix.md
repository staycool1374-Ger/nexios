# AUDIT REPORT 2026-11-17-580-release-build-fix
PATCH: audits/pending_patch.diff
FILES: src/kernel/sync/spinlock_order.hpp

## FINDINGS
- [S3] src/kernel/sync/spinlock_order.hpp:159 — forward declaration omits `inline` while the definition at :167 carries it
  WHY: Well-formed as written (non-inline decl before inline def is legal; the ill-formed direction is def-before-first-inline-decl), but spelling `inline` on the declaration would match Translation-Unit intent symmetrically and remove all doubt; cosmetic only, no behavior or ODR effect.

## CHECK DISPOSITION (all 11 critical checks)
1. Dynamic allocations: none added (comment + declaration only). PASS.
2. Concurrency boundaries: no lock, IRQ-window, rank-table, or failure-policy change; zero behavior delta. PASS.
3. Assertion masking: no test files touched; no assertions changed (build-only defect by design). PASS.
4. Memory safety: no allocation/free/load/store added. PASS.
5. Critical-section interference: the added declaration introduces no code, state, or ordering change. PASS.
6. Preprocessor semantics: insertion sits inside the `#else // !CONFIG_DEBUG` branch (hpp:143) before first use at :163; debug branch (:51-141) byte-untouched; both branches now declare the identical signature `uint64_t spinlock_held_by_current_count() noexcept` (matches hpp:88); per-build single definition holds — debug uses cpp:208 (inside `#if defined(CONFIG_DEBUG)`, :142-395), release uses the header inline at :167-169 over `spinlock_held_depth_own()` (cpp:128, outside any gate). Non-inline fwd-decl followed by inline def is well-formed. CONFIG_LOCK_DENIED_SPIN_STI_ALLOW lives in park_lock.cpp only — untouched. PASS.
7. Retrieval artifacts: delegating prompt carries on-thread CHECK-7 block (graphify query + disposition, vault search 0 hits, no precedent to honor/contradict). Independent GitHub-thread verification impossible with granted tool set (no GitHub MCP); accepted as in-session artifact. Prior-art check: LEARNINGS.md #347-B documents the same defect family (release compile break across the CONFIG_DEBUG boundary) — this patch applies that lesson correctly, not a repeat violation. PASS with noted tool limitation.
8. Multi-arch parity: shared arch-independent header, no arch gates touched; same declaration reaches x86_64/aarch64/riscv64. PASS.
9. Fail-closed: no hardware/DMA/alloc/IPC path in scope. N/A.
10. Capability isolation: no cross-domain channel, no shared state added. N/A.
11. Evidence: developer-supplied claims recorded — x86_64/aarch64/riscv64 release builds link green (reproducer fixed), x86_64 debug rebuild green, style Errors 0, release smoke x86_64 selftest 87/87 (test-history.txt tail confirms `2026-11-17 21:20:14 selftest PASSED: 87 FAILED: 0`; release ipc stages 0/0 consistent with TF_RELEASE link-boot shape). Test execution is developer-owned and auditor-forbidden; static scope confirms 4 insertions / 0 deletions in one non-test file. PASS.

## SCOPE CONFIRMATION
audits/pending_patch.diff is 4 insertions, 0 deletions, single file (src/kernel/sync/spinlock_order.hpp). No test files in scope by design. Worktree header already contains the patch content; diff and file agree (hpp:157-159 comment + declaration, byte-identical signature to hpp:88).

DECISION: APPROVED
