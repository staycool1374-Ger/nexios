# AUDIT REPORT 2026-09-03T162210Z
PATCH: audits/pending_patch.diff
FILES: docs/specs/syscall-fastpath.md, src/kernel/nexios_config.h, src/kernel/syscall/syscall.cpp, src/kernel/syscall/syscall.hpp, src/kernel/task/scheduler.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_syscall_fastpath.cpp, src/lib/test.hpp, userspace/yield-probe.c

## FINDINGS

- [S2] src/kernel/task/scheduler.cpp:1617 (canary tick sample) + src/kernel/elf/elf.cpp:723-779 (interaction, pre-existing file NOT in patch) — **exec-window false canary trip.** In `exec_into_current`, `tcb->page_table_` is swapped to the new pml4 (line 723) but the new image's canaries are installed only at `install_segment_canaries` (line 779). Between these two points the TCB's `canary_before/after/installed` still describe the OLD image while `page_table_` is the NEW pml4. The relocated scheduler hook `canary_check_in_scheduler_hooks(current_task(), 0)` — reachable from the on_tick tick sample (gated only by `is_user_`, `magic==TCB_MAGIC`, and the 1/64 tick cadence; the 0x80 syscall runs as a trap gate with IF preserved, so a nested timer ISR can fire mid-exec) — walks the OLD canary VAs against the NEW pml4 via `canary_verify_user_segments`; the old VAs are unmapped there (`virt_to_phys_in_pml4` → 0) so the verify returns false → spurious production panic or spurious test-mode latch on a legitimate exec.
  WHY: The canary relocation moved verification into scheduler hooks that can execute DURING the exec page-table/canary swap window, where the TCB canary fields are transiently inconsistent with `page_table_`, producing a false canary violation.

- [S3] src/kernel/test/test_syscall_fastpath.cpp:100-104 — Testdoc claims "compile-time static_asserts" but no `static_assert` exists in syscall.hpp; the invariant is enforced only at runtime by `fast_mask_matches_config`.
  WHY: Doc/implementation mismatch (the spec §3.1 also claims a `static_assert`); the runtime test does cover the invariant, so no functional gap.

- [S3] src/kernel/syscall/syscall.cpp:160-167 — `handle_fast` omits the debug AC check that `handle` keeps on both branches; `handle_fast` is currently test-only and unreachable from the production `syscall_handler` path.
  WHY: No production exposure today, but the spec's "AC check stays on both paths" claim is not literally true for `handle_fast`, a footgun if a future asm entry adopts it.

## PATCH
audits/rejected_patch.diff was written (git-apply-able, verified with `git apply --check`): clears `tcb->canary_installed = 0` immediately before the `page_table_` swap in `exec_into_current`, making `canary_verify_user_segments` a no-op across the swap→install window so the relocated scheduler canary sampling cannot false-trip on stale canary fields; `install_segment_canaries` re-ORs the new image's bits.

DECISION: REJECTED