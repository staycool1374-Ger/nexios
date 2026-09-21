# AUDIT REPORT 2026-09-03T163438Z (iteration 2)
PATCH: audits/pending_patch.diff
FILES: docs/specs/syscall-fastpath.md, src/kernel/elf/elf.cpp, src/kernel/nexios_config.h, src/kernel/syscall/syscall.cpp, src/kernel/syscall/syscall.hpp, src/kernel/task/scheduler.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_syscall_fastpath.cpp, src/lib/test.hpp, userspace/yield-probe.c
PREVIOUS: report-20260903T162210Z.md (REJECTED, one S2 + two S3)

## FINDINGS

### S2 (prev) — exec-window false canary trip: REMEDIATED (verified closed)
elf.cpp:733 clears `tcb->canary_installed = 0` immediately BEFORE the
`tcb->page_table_ = new_pml4` swap at line 734. Verified by direct source read:

- Clear-before-swap ordering confirmed (elf.cpp:733 precedes 734).
- `canary_verify_user_segments` is a hard no-op when `canary_installed == 0`
  (task.cpp:826-828 skips every segment) → `canary_check_in_scheduler_hooks`
  returns true across the swap→install window; neither the on_tick sample nor
  the context-switch hook can false-trip on stale canary fields.
- `install_segment_canaries` (elf.cpp:790) re-ORs TEXT/DATA bits (662, 675) and
  calls `canary_install_user_segments` (ORs STACK/HEAP, task.cpp:798/810) and
  `canary_install_kernel_stack` (ORs kernel bit, task.cpp:819). All bits cleared
  by the fix are restored by OR, not overwrite — the TCB returns to a
  fully-armed, consistent state before the task is schedulable again.
- No OTHER live `page_table_` swap exists for a schedulable user task:
  construction paths set page_table_ before the task is added to the registry
  (finalize_loaded_task elf.cpp:531, fork task.cpp:1500 with canaries re-armed
  at 1569-1575 before the child is returned), and teardown zeroes
  `page_table_` only after removal (task.cpp:1863). exec_into_current is the
  sole runtime swap for a live task and is now closed.
- The context-switch hook cannot fire mid-exec (the exec-ing task is the
  physical runner); the only reachable sample is on_tick, gated by
  lock_acquired + `(current_tick & 63) == 0`, and is inert during the window.
- Empirical: post-fix re-runs green — syscall_fastpath 5/5, memory_safety
  11/11, process_elf 9/9, elf_loader 8/8 (test-history.txt 18:30:16-18:31:17).

### S3 (prev) — testdoc/spec claim compile-time static_asserts that do not exist: REMAINS OPEN
No `static_assert` exists in syscall.hpp (grep confirms zero in the syscall
headers); spec §3.1 and the testdoc still claim one. The invariant (mask
non-empty, all bits < MAX_SYSCALL, popcount == list size, no stray bits) is
fully enforced at runtime by `fast_mask_matches_config` (the test class runs
in every gate). Functional gap: none. Doc/implementation mismatch only.

### S3 (prev) — handle_fast omits the debug AC check: REMAINS OPEN
`Syscall::handle_fast` has no `read_rflags() & (1<<18)` AC check. The
production entry path is `Syscall::handle`, which checks AC before the FAST
branch (syscall.cpp:129-138), so MP-4 detection is intact on both production
branches. `handle_fast` is reachable only from tests today. Footgun if a
future asm entry adopts it; documented in the spec as a deviation.

### S3 (new) — probe-based tests silently PASS if the probe ELF is absent
`full_path_still_validates` / `fast_path_skips_canary` return `JARVIS_TEST_PASS`
when `load_probe(...)` yields nullptr. Verified non-vacuous today:
`mk/rules.mk:107` wildcards `userspace/*.c` and the CPIO rule (186-192) bundles
every `*.c.elf`; `yield-probe.c.elf` and `user-app.c.elf` exist as build
artifacts, and the class runs green. Mirrors the pre-existing memory_safety
convention. Residual fragility: a broken probe build would silently vacate the
canary tests. Not blocking.

## CRITICAL CHECKS (full patch re-run)

1. FAST-mask membership security — PASS. Mask = {YIELD 0, PRINT 4, GET_TICKS 5,
   CREATE_MAILBOX 7, DESTROY_MAILBOX 8, GETPID 23, PAUSE 36, REBOOT 49,
   HALT 50}. SEND(1)/RECEIVE(2)/SEND_SYNC(3) excluded. Handlers read in source:
   none dereferences a user pointer (sys_yield: io_wait+reschedule;
   sys_print/create/destroy_mailbox: return 0; sys_get_ticks/getpid: kernel
   reads; pause/reboot/halt: no user access). Kernel-pointer reads (e.g.
   syscall_task) are not user derefs and are safe without canary gating.
2. Canary relocation correctness — PASS. Pure page-table walks via HHDM
   (virt_to_phys_in_pml4, vmm.cpp:1124: no locks/alloc); ISR-context safe;
   TCB_MAGIC guard on both hook sites; test-mode latch via is_test_active()
   vs production panic preserved; magic guards intact.
3. FULL-path canary still fires — PASS. memory_safety 11/11 post-fix;
   full_path_still_validates latches (context-switch/tick or FULL syscall,
   either path).
4. Debug/release symmetry — PASS. CONFIG_CANARY_GUARD=1 default both builds;
   tick sample + context-switch hooks unconditional on build (function body
   guarded internally); AC check is debug-only on `handle` for both FAST/FULL
   branches. handle_fast unreachable in production.
5. Bounds-check-before-shift — PASS. handle() checks `number >= MAX_SYSCALL`
   (line 126) before `1ULL << number` (145); handle_fast checks before table
   index (162-163). Max shift 62 < 64.
6. No Heisenbug masking in fast_latency_lt_full — PASS. Relative
   sum-comparison with avg_fast <= 2*avg_full headroom; correctness proven
   functionally by fast_call_correctness (both paths identical) and
   fast_path_skips_canary/full_path_still_validates, not by the latency test.
7. Scheduler hot-path cost bounded — PASS. Context-switch canary verify is a
   bounded read (≤ 8 page-walks per user switch); tick sample 1/64 ticks
   (mask check, power-of-two). Both documented in the spec.
8. No dynamic allocation in IRQ/syscall paths — PASS. Canary hooks and the
   FAST dispatch perform no allocation/locking.

## PATCH
audits/rejected_patch.diff NOT written — no S1/S2 findings. The iteration-1
S2 corrective change (elf.cpp canary_installed clear) is confirmed in the
pending patch and fully closes the exec-window false-trip window.

DECISION: APPROVED