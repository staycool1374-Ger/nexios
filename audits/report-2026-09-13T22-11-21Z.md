# AUDIT REPORT 2026-09-13T22-11-21Z
PATCH: audits/pending_patch.diff
FILES: Makefile, mk/rules.mk, src/kernel/arch/x86_64/acpi.cpp, src/kernel/arch/x86_64/boot/ap_trampoline.nasm, src/kernel/arch/x86_64/hal/apic.cpp, src/kernel/arch/x86_64/hal/apic.hpp, src/kernel/arch/x86_64/hal/percpu.hpp, src/kernel/arch/x86_64/hal/smp.cpp, src/kernel/arch/x86_64/hal/smp.hpp, src/kernel/arch/x86_64/madt.hpp, src/kernel/kernel.cpp, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_registry.cpp, src/kernel/test/test_smp_bringup.cpp, src/kernel/test/test_smp_ipi.cpp, src/kernel/test/test_smp_madt.cpp

## FINDINGS
- [S3] src/kernel/arch/x86_64/hal/smp.cpp:607 — volatile rendezvous flags (ap_ready) used for cross-CPU sync instead of atomics
  WHY: CODING_STYLE §6 forbids volatile-for-sync; single-writer-per-slot + x86 TSO + volatile reload-per-iteration make it deterministically correct here, so hardening only (std::atomic<uint8_t>).
- [S3] src/kernel/arch/x86_64/hal/smp.cpp:757 — AP rendezvous loop is unbounded when tsc_freq_hz()==0
  WHY: The 1 s TSC bound vanishes if freq==0, hanging boot with IRQs disabled; unreachable today (Timer::init falls back to 2 GHz, timer.cpp:142, and bring_up runs after Timer::init), but an explicit fail-closed panic on freq==0 would close it.
- [S3] src/kernel/arch/x86_64/hal/smp.cpp:617 — g_block_reserved set unconditionally although PMM::reserve_range silently skips already-allocated pages
  WHY: Verified unreachable (no PMM alloc precedes reserve: VMM::init allocates nothing, window base is 0 so 0x70000 is in-window) but future code motion inserting an alloc before reserve would make the bring_up gate vacuous; verifying the bit stuck would harden it.
- [S3] src/kernel/arch/x86_64/boot/ap_trampoline.nasm:278 — 32-bit CR3 load requires the kernel PML4 to sit below 4 GiB, undocumented
  WHY: `mov eax,[pml4_phys]` truncates any PML4 above 4 GiB (true by construction today — boot tables live in the low image — and inherent to any 16→32→64 trampoline), but the constraint is nowhere stated next to PARAM_PML4.
- [S3] src/kernel/arch/x86_64/hal/smp.cpp:762 — AP-start-timeout path never logs the LAPIC ID value despite the "naming" claim
  WHY: Two static warn strings + panic carry no ID (Logger::warn is printf-style, logger.hpp:52, so `warn("...LAPIC ID 0x%x", target)` is a one-liner); diagnosability only.
- [S3] mk/rules.mk:31 — comment states the trampoline executes at "physical 0x7000"
  WHY: Off by 16x — SIPI vector 0x70 starts the AP at 0x70000 (correct everywhere else: nasm org, TRAMPOLINE_ADDR, test asserts); comment-only typo in safety-relevant documentation.

## CHECKS SUMMARY
1. Dynamic allocations: PASS (static BSS + stack only; no new/delete/malloc/free).
2. Concurrency boundaries: PASS (bring_up fully under IrqGuard; TSC-only waits with no IRQ/tick dependency; AP writes own slot/own PerCpu only; mode_ written-before-SIPI).
3. Assertion masking: PASS (new tests only; end-to-end delivery flag, exact blob-size memcmp, ap_count==snapshot assertions; `if(readable)` closed by trailing JARVIS_ASSERT(readable)).
4. Memory safety: PASS (no PMM alloc/free surface; reserve-then-copy with usable+reserved gates and exact-size check; HHDM test reads allocate nothing; mb2 drop follows acpi_parse precedent).
5. Critical-section interference: PASS (enable_local register set byte-identical to extracted code; init_ap mirrors BSP APIC_BASE mode per-CPU before any x2 MSR touch with no IOAPIC contact; lapic_id x2 fix correct; ICR encodings 0x4500/0x8500/0x4600|vec verified; bring_up after Timer::init, APs hlt-park with scheduler BSP-only; percpu_init_ap bounds-checked).
6. Preprocessor/conditionals: PASS (x86 guards pair at all 3 registry sites and kernel.cpp; .nasm dodges the generic .asm rule; EXTRA_LINK_OBJ in both link lines with empty else-branch; -smp 2 scoped to CLASS=smp_bringup; MADT cap == CONFIG_MAX_CPUS via static_assert).
7. Retrieval artifacts: PASS-WITH-NOTE (issue #25 shows the graphify APIC/X2APIC/AP-startup/per-CPU/GDT/TSS/IPI/bring-up query with per-cpu-smp.md + vault-search-0-matches dispositions; no Phase-B-fresh paste exists, but the posted query explicitly covers Phase B topics and the design doc drove this patch — reuse accepted, fresh paste recommended for Phase C).

DECISION: APPROVED
