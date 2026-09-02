# AUDIT REPORT 2026-08-25T16-00:00Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/syscall/syscall.cpp, src/kernel/syscall/syscall_entry.asm, mk/rules.mk, src/kernel/test/test_idt.cpp, src/kernel/test/test_isolate.cpp

## FINDINGS
- [S3] src/kernel/syscall/syscall.cpp:30-53 — Syscall::init() documented no-op for x86_64. IA32_STAR/IA32_LSTAR/IA32_FMASK wrmsr writes removed; init() now a documented no-op with comment explaining the fastpath redesign defers to docs/specs/syscall-fastpath.md (v0.4.3). Rationale: MSR_KERNEL_GS_BASE was never written, so swapgs left GS base 0 → guaranteed panic on ring-3 syscall (0F 05). Sole live path is int $0x80 (isr_128 trap gate, GS-free).
- [S3] src/kernel/syscall/syscall_entry.asm:64-74 — File marked DEAD CODE, not assembled. Comment-only historical artifact. Filter-out moved outside the `ifneq ($(ARCH),x86_64)` block in rules.mk so it is unassembled on ALL architectures. Retained as commented historical pending explicit deletion approval.
- [S3] mk/rules.mk:13-18 — `SRC_ASM_GENERIC` filter-out for `src/kernel/syscall/syscall_entry.asm` moved OUTSIDE the `ifneq ($(ARCH),x86_64)` block. Previously only excluded on non-x86 architectures; now unconditionally excluded on all arches. Intentional per P7: the LSTAR/sysret fastpath was removed, making the file dead code on all architectures.
- [S3] src/kernel/test/test_idt.cpp:104-110 — `idt_syscall_handler_installed` test updated: assertion direction corrected from `lstar != 0` to `lstar == 0` (fastpath removed, LSTAR must now be 0). New assertion `IDT::has_handler(0x80)` added to verify the live int $0x80 trap gate is installed. Companion edit mandated by the LSTAR removal (test-suite P2 item).
- [S3] src/kernel/test/test_isolate.cpp:130-136 — `snapshot_create()` added `ENSURE(!arch::interrupts_enabled())` under `#if CONFIG_DEBUG`. The IrqGuard already guarantees IF=0 on entry; ENSURE catches a nested guard that silently re-enables IRQs. ENSURE panic-on-violation cannot fire spuriously in release builds (code excluded via #ifdef). In debug mode, the IrqGuard guarantee + ENSURE forms a correct defense-in-depth pattern.

## PATCH
audits/rejected_patch.diff was NOT written; all changes pass verification.

## DECISION: APPROVED
