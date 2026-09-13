# AUDIT REPORT 2026-09-13T17-27-10Z
PATCH: audits/pending_patch.diff
FILES: linker/linker_x86_64.ld, src/kernel/arch/cpu_context.hpp, src/kernel/arch/x86_64/hal/apic.cpp, src/kernel/arch/x86_64/hal/apic.hpp, src/kernel/arch/x86_64/isr_stubs.asm, src/kernel/core/global_state.cpp, src/kernel/kernel.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_per_cpu.cpp, tools/validate_style.py

## FINDINGS
- [S3] linker/linker_x86_64.ld:17 — absolute aliases depend on Itanium-mangled `_ZN4arch7per_cpuE` and frozen offsets 0x20/0x28/0x30
  WHY: Any namespace/type rename silently breaks the alias, but PerCpu layout was verified field-by-field (0x00/0x08/0x10/0x18/0x20/0x28/0x30/0x38, 4096 B) and test_per_cpu asserts alias identity at runtime, so this is hardening-only.
- [S3] src/kernel/arch/x86_64/hal/apic.cpp:186 — `APIC::lapic_id()` applies `>> 24` to the x2APIC ID path identically to xAPIC
  WHY: x2APIC MSR 0x802 carries the full 32-bit ID in bits 31:0 (xAPIC MMIO carries it in 31:24), so the shift truncates large x2APIC IDs — harmless in single-core Phase A (BSP ID 0, value stored but unconsumed), fix before SMP use.
- [S3] tools/validate_style.py:407 — PerCpuAsmChecker patterns are case-sensitive and cover only isr_nesting_depth/irq_entry_tsc
  WHY: Uppercase `[REL ISR_NESTING_DEPTH]` or a future `[rel fpu_owner]` would evade the gate, but current asm uses lowercase and Phase A asm never touches gs:0x30, so no present-tense bypass.

Checks 1–7 disposition: (1) no heap allocation in patch — PASS; (2) test sentinel window under IrqGuard with restore-before-assert, asm RMW discipline unchanged single-core — PASS; (3) expected-counts adds one new row only, no existing assertion touched — PASS; (4) no PMM/BufferPool code touched — PASS; (5) GS_BASE set at kernel.cpp:539 before IDT init/load (593–594) with zeroed slots and no sti/enable before, C++ extern-C linkage matches linker symbols, depth-guard immediates/widths and r8 entry_tsc feed identical — PASS; (6) CONFIG_ARCH_X86_64 guards symmetric across global_state.cpp/kernel.cpp/tests, non-x86_64 keeps .bss definitions, CpuContext field deletion has zero tree users — PASS; (7) graphify query + vault search artifacts pasted on issue #25 thread per developer context — PASS.

DECISION: APPROVED
