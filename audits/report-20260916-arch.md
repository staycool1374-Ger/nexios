# AUDIT REPORT 20260916-130230
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/aarch64/timer.cpp, src/kernel/arch/hal/timer.hpp, src/kernel/arch/riscv64/timer.cpp, src/kernel/arch/x86_64/hal/apic.cpp, src/kernel/arch/x86_64/hal/apic.hpp, src/kernel/arch/x86_64/hal/hpet.cpp, src/kernel/arch/x86_64/hal/hpet.hpp, src/kernel/arch/x86_64/hal/timer.cpp, src/kernel/nexios_config.h, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_hr_clock.cpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_weak_stubs.cpp

## FINDINGS
- [S3] src/kernel/arch/x86_64/hal/timer.cpp:267 — plain (non-atomic) active_source_ read/write across ns_monotonic()/calibrate() refresh path
  WHY: Single-byte boot-stable label with only a TSC→TSC_DEADLINE promotion means any stale read is transient and frequency-identical, so hardening to explicit atomic access is optional.
- [S3] src/kernel/arch/x86_64/hal/hpet.cpp:40 — CONFIG_HAS_HPET conditional has identical fail-closed outcomes on both branches (present_ = false)
  WHY: Dead conditional is intentional until ACPI/VMM mapping exists and never dereferences HPET_PHYS_BASE, so it is a forward-compat stub rather than a behavior divergence.
- [S3] src/kernel/arch/x86_64/hal/apic.hpp:57 — Timer::oneshot()/APIC::arm_deadline() steal the LAPIC timer (set_timer_oneshot clears periodic re-arm) with restore assigned to the caller
  WHY: Steal-and-restore discipline is documented in the header contract and honored by the test (timer_init + timer_start before further asserts) with no production caller yet, so the residual risk sits with the future #17 wheel, not this patch.

DECISION: APPROVED
