# AUDIT REPORT 2026-09-21T16-45-00Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/test/test_syscall.cpp, src/kernel/arch/aarch64/test_aarch64.cpp, src/kernel/arch/riscv64/test_riscv64.cpp, src/kernel/test/test_expected_counts.hpp (plus self-evicting header hunk removing stale #104 content from the tracked pending file — no new production change)

## FINDINGS
- [S3] src/kernel/test/test_expected_counts.hpp:arch_riscv64 row — enabling validation on a previously-unvalidated class ({0,0,0}->{0,0,3}) changes gate behavior: any pre-existing drift in that class now fails the gate
  WHY: 0 disables validation so nonzero counts newly arm a gate that never enforced before; flagged risk only, counts themselves are arithmetically correct (+3 registered tests).
- [S3] GitHub issue #30 thread — graphify + obsidian retrieval artifacts asserted by developer but unverifiable by auditor (no browse capability)
  WHY: Task instruction explicitly states this is not a reject reason alone; recorded per instruction, PROMPT-audit.md check 7 noted without S2 escalation.

## VERIFIED (no finding — evidence-backed)
- x86 bridge: isr_stubs.asm:140-154 pushes r15..rax (15 pushes, rax last => regs[0]=rax); regs[22] frame with regs[17]=rip regs[18]=CS matches; handle_interrupt_c(0x80,...) skips vector<32 path (kernel.cpp:1664), #NM(7), reserved vectors, reaches ONLY the 0x80 block (1778-1801) which writes ONLY regs[0] (1779-1780); TPR prologue is read-mostly + APIC-is_enabled-guarded (1548-1562); pending-signal check skips kernel tasks via is_user_ (1784); reschedule only on TERMINATED/BLOCKED (1796-1799) — GETPID/KILL probes never terminate; regs[0]-restore-before-redispatch fix is correct and complete.
- KILL routing: SIG_NONE==0 (src/lib/signal.hpp:31); sys_kill bounds-checks sig>=MAX(32) first (252), SIG_NONE short-circuits to 0 before find_task (256-257 vs 258); KILL(999999,1)->-1 via find_task failure with no state mutation; KILL(999999,0)->0 with no lookup/delivery; no self-KILL used.
- aarch64: save layout sp+0=x0..sp+64=x8 => frame[8]=x8, frame[0..3]=x0..x3; extraction ldr x0,[sp,#64] + ldp x1,x2,[sp,#0] + ldp x3,x4,[sp,#16] (syscall_entry.S:57-59) replicated exactly; riscv: OFF_A0=72->9, A1=80->10, A7=128->16 with extraction ld a0,OFF_A7 / a1,OFF_A0 / a2,OFF_A1 (syscall_entry.S:164-168) replicated exactly; syscall_handler is a 1-line forward to Syscall::handle (kernel.cpp:1863-1866) so direct-handle dispatch loses no issue-scope coverage; GETPID null-safe (t?id:0) matches cur?id:0 expectation; canary_check_on_full_path skips kernel/null tasks (syscall.cpp:67-68), fabricated regs only read for rip on trip path — safe for GETPID/KILL.
- GIC fix: GIC_MAX_IRQ=64 with early return in mask/unmask (interrupt_controller.cpp:100-124) => IRQ 64 never touches MMIO, old test could never pass, driver correct per #198; IRQ 32 in-window SPI, bank=1 bit=0 => gicd[0x100/4+1] bit 0 correct; eoi(32) is a stateless EOIR write (76-82), order-independent with mask/unmask.
- Counts: syscall_core 29->32 with identical CONFIG_ARCH_X86_64 guards on tests + registration; arch_aarch64 23->26; arch_riscv64 ->3; core aggregate includes run_syscall_core_group (test_registry.cpp:1002) so 436->439 arithmetically required.
- No production code changed (live hunks are test files + counts header only); TEMP-token sweep of patch is zero; critical checks 1-6 clean (no heap allocs, no new lock boundaries, no assertion masking, no PMM/double-free risk, no critical-section interference, preprocessor guards symmetric).

DECISION: APPROVED
