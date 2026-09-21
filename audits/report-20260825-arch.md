# AUDIT REPORT 2026-08-25T15:05:22Z
PATCH: audits/pending_patch.diff
FILES: src/kernel/arch/aarch64/early_init.cpp, src/kernel/arch/aarch64/hal/io_impl.hpp, src/kernel/arch/aarch64/hal/page_table_impl.hpp, src/kernel/arch/aarch64/test_aarch64.cpp, src/kernel/kernel.cpp, src/kernel/memory/vmm.cpp, src/kernel/memory/vmm.hpp, src/kernel/nexios_config.h, src/kernel/test/test_expected_counts.hpp, src/kernel/arch/riscv64/hal/io_impl.hpp

## FINDINGS
- [S1] — — (none)
- [S2] — — (none)
- [S3] src/kernel/arch/aarch64/early_init.cpp:23 — g_pan_supported plain bool write-once at boot, read-only thereafter; safe per codebase conventions, no lock/atomic needed
  WHY: Write-once pattern during boot with IRQs masked; read-afterwards from any context has no data race per codebase conventions
- [S3] src/kernel/arch/aarch64/hal/io_impl.hpp:86-113 — stac/clac/read_rflags gated on g_pan_supported; no PAN sysreg access when FEAT_PAN unsupported
  WHY: Every inline accessor checks g_pan_supported first; degraded mode matches old no-op stubs, no incorrect PAN state possible
- [S3] src/kernel/arch/aarch64/hal/page_table_impl.hpp:128-131 — attr_from_flags sets PXN on USER pages, UXN|PXN on kernel pages; SMEP parity closure
  WHY: PXN|UXN correctly propagated per ARMv8-A descriptor semantics; CONFIG_ARCH_AARCH64-gated
- [S3] src/kernel/arch/aarch64/test_aarch64.cpp:162-188 — walk_leaf_descriptor uses DESC_TABLE (0b11) verification; L3 fix correct per ARMv8-A
  WHY: L3 leaf descriptor encoding changed from reserved DESC_BLOCK (0b01) to DESC_TABLE (0b11); no impact on x86_64 or riscv64 paths
- [S3] src/kernel/kernel.cpp:580-355 — pan_init() called at higherhalf_entry with debug_write; gates on CONFIG_ARCH_AARCH64 && CONFIG_PAN
  WHY: Boot-time PAN enablement; no runtime path executes SCTLR_EL1.PAN or S3_0_C4_C2_4 when FEAT_PAN unsupported
- [S3] src/kernel/memory/vmm.cpp:369-391,556-583 — base_flags propagate PAGE_UXN|PAGE_PXN; map_page/map_page_in_pml4 set PXN on user pages, UXN|PXN on kernel pages
  WHY: PXN/UXN closure across page table walks; all aarch64 edits gated on CONFIG_ARCH_AARCH64
- [S3] src/kernel/memory/vmm.hpp:430-431 — PAGE_PXN (1ULL<<53) and PAGE_UXN (1ULL<<54) constants defined; comment corrected for bit positions
  WHY: New flag bits for PXN/UAXN closure; consistent with ARMv8-A bit 53=PXN, bit 54=UXN
- [S3] src/kernel/nexios_config.h:456-462 — CONFIG_PAN default 1 on aarch64; conditional define
  WHY: Ensures CONFIG_PAN is set appropriately for aarch64 builds; no effect on other architectures
- [S3] src/kernel/test/test_expected_counts.hpp:476 — arch_aarch64 expected count 0 → 22; 5 new MP-4.4 PAN/PXN tests added
  WHY: Test count updated to reflect 5 new aarch64 architecture tests; no test results committed in this patch
- [S3] src/kernel/arch/riscv64/hal/io_impl.hpp:332-334 — comment-only change noting aarch64 PAN model for future SSTATUS.SUM port
  WHY: RISC-V change is comment-only; no code functional impact

## PATCH
audits/rejected_patch.diff was not written; all findings are S3-only.

## DECISION: APPROVED
