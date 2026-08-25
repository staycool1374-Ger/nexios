# LEARNINGS — Knowledge Base (FEEDBACK transition artifact)

> Appended after every completed workflow cycle (state machine: FEEDBACK).
> Each entry records what the cycle taught, so background knowledge is
> adapted into the working context instead of being lost between sessions.
> The gate (`scripts/gate.py check-feedback <issue>`) verifies that a closed
> issue has a matching entry here — the FEEDBACK transition is not complete
> without it.

## Entry format

```
### #<issue> — <short title> (<date>)
- **Learned:** what new insight/technique/pitfall emerged this cycle
- **Adapted:** where this knowledge now lives (spec, AGENTS.md, code comment)
- **Style re-surface:** which CODING_STYLE.md rules were relevant and must stay in focus
```

## Entries

<!-- Append new entries below; newest first. -->

### #6 — Implement all pending audits/refactorings under audits/ (2026-08-25)
- **Learned:** (1) The single most reusable kernel-blocking pattern is "release the primitive lock BEFORE reschedule/dequeue_ready" — holding a spinlock across the INV-4 deferred switch deadlocks the ISR-side waker; every blocking path (semaphore/eventgroup/queue/mutex) must mirror Queue/Notify: guard scoped to waiter-insert, then dequeue_ready + reschedule outside, with an interrupts-disabled rollback (remove waiter, RUNNING, enqueue_ready). (2) Generation-tagging bare TCB pointers (waiter_, last_sender_/last_receiver_) defeats ABA from recycled TCBs — check REAPED state AND captured generation, but allow TERMINATED zombies (still allocated) so PIP boosts on recently-terminated holders keep working. (3) The custom Logger reads ALL integers as uint64_t and previously mishandled `%llu` (skipped only one 'l'), leaving the va_arg list misaligned for a following `%s` → crash only on the H2-flake diagnostic branch. (4) The LSTAR/sysret path was a guaranteed-panic landmine (MSR_KERNEL_GS_BASE never written → swapgs left GS base 0 → mov [gs:0],rsp #PF on phys 0); int $0x80 is the only safe syscall entry. (5) Priority-inheritance restore must use strict `>` (not `>=`) or the holder_priority_ latch never clears → permanent priority inflation. (6) Endpoint dispose must drain blocked senders itself — the embedded MessageQueue dtor never runs under MemPool::free. (7) `%u` with a 32-bit cast on a 64-bit id in the custom logger leaves garbage high bits.
- **Adapted:** 8 SIL-3-approved commits (P1–P8): semaphore/eventgroup lock-scope + dequeue_ready; notify/queue generation-tags + PIP re-bucketing via move_priority; TCB::destroy() pool-aware helper; scheduler alloc_id table-full contract + atomic next_task_id_ + validate_iret_frame helper + task_stack_ptr inline; BufferPool map owner/VA/TLB-flush; Endpoint dispose drain + disposed_ re-check; AHCI/virtio serialization + GHC_IE removal; LSTAR removal (syscall.cpp no-op + mk/rules.mk unassembled + test_idt companion); Mutex H-4 retry; TaskQueue orphan-drop; SPSC atomics. Each phase has an immutable audit report in audits/. Full debug `all` 932/932, release selftest 85/85.
- **Style re-surface:** CODING_STYLE §5 (ENSURE only for impossible invariants; reachable exhaustion → error/retry); §4 (no new/delete on RT paths → destroy() helper); no spinlock across a context switch; IrqGuard only for boot/panic/test-isolation (ROADMAP guardrails); test sanctity (doc-block + implementation updated together when a fix changes expected behavior).

### #99 — Multi-arch compile-clean: aarch64 + riscv64 link green (2026-08-25)
- **Learned:** (1) Cross-arch builds with a shared `build/` require the arch-switch clean at PARSE TIME, before `-include $(DEPFILES)` — a mid-build clean (rule prerequisite) is too late because make commits to "object up to date" from the previous arch's `.d` files and never rebuilds it (caused `cannot find build/initrd/initrd.o`). (2) The old `gen_test_registry.py` had a legacy truncation bug: its `#else` depth arithmetic left `disabled_depth` elevated, silently dropping the tail of files (test_stack_alloc.cpp lost 5 real tests from the dormant `generated_tests[]`). (3) AArch64 `ADR_PREL_PG_HI21` can't reach low-VMA symbols from higher-half `.text`; higher-half boot-stack symbols must be linker absolute symbols (`HHDM + low_vma`). (4) riscv64 inline asm uses `mv` (no `mov`); aarch64 uses `mov %0, sp`.
- **Adapted:** Makefile parse-time arch-stamp check + per-arch `mk/cpp-rules.$(ARCH).gen.mk`; `tools/gen_test_registry.py` arch-aware stripping (SCAN/DROP/ARCH mode stack); `linker/linker_aarch64.ld` `_stack_start`/`_stack_end` absolute symbols; scheduler.cpp/task.cpp `CONFIG_ARCH_X86_64` guards.
- **Style re-surface:** CODING_STYLE preprocessor-guard placement (whole contiguous statement groups, x86_64 path byte-identical); minimal-diff discipline; Werror-clean.

### # — gate test entry (2026-08-23)
- **Learned:** gh --jq returns raw text, not JSON
- **Adapted:** scripts/gate.py
- **Style re-surface:** none
