# AUDIT REPORT 2026-09-01T22-04-40Z
PATCH: audits/pending_patch.diff
FILES: Makefile, docs/specs/iommu.md, src/kernel/arch/x86_64/acpi.{hpp,cpp}, src/kernel/iommu/dmar.hpp, src/kernel/iommu/iommu.{hpp,cpp}, src/kernel/iommu/vtd.hpp, src/kernel/kernel.cpp, src/kernel/nexios_config.h, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_iommu_live.cpp, src/kernel/test/test_registry.cpp

## FINDINGS

- [S1] src/kernel/iommu/vtd.hpp:60, iommu.cpp:380,387,727 — spec §9.3 context-entry Translation-Type field mis-encoded
  WHY: The legacy context-entry TT field (bits 3:2) per the VT-d spec §9.3 / QEMU (`VTD_CONTEXT_TT_MULTI_LEVEL=0`, `PASS_THROUGH=2<<2`) is a TWO-BIT type: 00b=second-stage translate through ASR, 01b=Device-TLB, 10b=pass-through. The codebase models it as a single "T" bit (`kCteTranslate=1<<2`=01b=Device-TLB) and programs "T=0 passthrough" as `cte->lo = kCtePresent` (TT=00b), which QEMU/spec decode as SECOND-STAGE TRANSLATE with a NULL ASR — the exact inverse of the intended passthrough. Consequences on the test target: the passthrough pre-pass writes translate entries (TT=00b, SSPTPTR=0) instead of pass-through, and attach_device writes Device-TLB type (01b), which QEMU rejects when `dt_supported=false` (no `device-iotlb` in the Makefile). Tests pass only because they never drive actual DMA through a context entry — the live-protection semantics are inverted.

- [S3] src/kernel/arch/x86_64/acpi.cpp:57-63 — acpi_map() failure derefs VA 0
  WHY: The iter-1 fix returns 0 from `acpi_map()` on mapping failure, but the caller `phys_read8()` dereferences the returned VA unconditionally; a failed mapping derefs address 0 (identity-mapped to PA 0 in this kernel), silently reading garbage instead of failing the scan closed. Corrective patch guards the 0 return.

## PATCH

`audits/rejected_patch.diff` was written and `git apply --check` passes. Its intent:
- Correct the context-entry Translation-Type encoding: passthrough entries program TT=10b (`kCtePresent | kCteTtPassthrough`) and translate entries program TT=00b (`kCteTtTranslate`), with the S2 passthrough skip updated to skip any present entry whose TT field is not passthrough (so a T=1 translate entry can never be downgraded and a never-attached device still gets the T=0 fallback).
- Harden `phys_read8()` to fail closed on a failed page mapping.
- Update the spec text to document the two-bit TT semantics.

DECISION: REJECTED