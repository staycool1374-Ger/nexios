# AUDIT REPORT 2026-09-01T20-36-12Z
PATCH: audits/pending_patch.diff
FILES: Makefile, docs/specs/iommu.md, src/kernel/arch/x86_64/acpi.cpp, src/kernel/arch/x86_64/acpi.hpp, src/kernel/iommu/dmar.hpp, src/kernel/iommu/iommu.cpp, src/kernel/iommu/iommu.hpp, src/kernel/iommu/vtd.hpp, src/kernel/kernel.cpp, src/kernel/nexios_config.h, src/kernel/test/test_expected_counts.hpp, src/kernel/test/test_iommu_live.cpp, src/kernel/test/test_registry.cpp

## FINDINGS
- [S2] src/kernel/iommu/iommu.cpp:374 — "Skip devices already owned by a user domain" is documented but NOT implemented; the passthrough pre-pass unconditionally overwrites the context entry to T=0 (`cte->lo = kCtePresent`) for every AHCI/virtio BDF on bus 0.
  WHY: If `enable_translation()` is ever invoked after a user domain has attached an AHCI/virtio device (T=1 entry), the pre-pass silently downgrades it to T=0 passthrough — defeating that device's DMA isolation (the IOMMU's whole purpose) — and nothing enforces the "enable before any user attach" ordering. Fix: skip BDFs whose context entry is already present + translated (T=1).

- [S3] src/kernel/iommu/iommu.cpp:480 — `enable_translation()` TE-timeout path zeroes `g_mmio_base` but leaves `g_probe_result==1`, so a subsequent `probe_hardware()` (cached branch) restores `g_live`/`g_present` with `g_mmio_base==0` → `live_active()` permanently false while `probe()==true` (half-live state, no retry possible).
  WHY: `g_mmio_base` is a pure hardware fact; dropping it breaks the cache-restore contract. Fail-closed direction is safe (translation stays off), but the cached state is inconsistent. Corrective patch keeps `g_mmio_base` and clears `g_translation_live` on the TE-timeout path so a re-probe/re-enable can retry.

- [S3] src/kernel/iommu/iommu.cpp:434 — VER_REG validation `ver == 0 || (ver & 0xFF) == 0` rejects every real VT-d unit with minor version 0 (e.g. VER=0x100, major=1/minor=0, the common hardware case) while accepting QEMU's VER=0x10 (major=0/minor=16).
  WHY: The low byte is the minor version, so the check is inverted for real hardware — a real unit is fail-closed-rejected and live VT-d never engages. Safe direction, but a real-hardware compatibility defect for the phase-2 live goal; consider `ver != 0` with a major!=0 check that still accepts QEMU.

- [S3] src/kernel/arch/x86_64/acpi.cpp:143-148 — `acpi_map()` ignores the `VMM::map_page` result (void API); a page-table allocation failure on the scan path would leave the VA unmapped and the subsequent byte-read faults rather than failing closed.
  WHY: Impractical at boot (PMM is nearly empty) and the test re-probe reuses the cache, but the fail-closed claim in the comments is not fully implemented for the mapping step. Also `phys_sig(entry)` in `find_acpi_table` reads 4 bytes at each trusted entry without a length check (post-checksum trust is acceptable; flagged as hardening).

- [S3] src/kernel/iommu/iommu.cpp:406-430 — `probe_hardware()` runs `acpi::scan_dmar()` and `VMM::map_page` (MMIO page) INSIDE `g_lock_`; the scope states the MMIO map_page should be outside the lock.
  WHY: No deadlock is possible today (boot is single-threaded; the test re-probe hits the cache branch and never re-maps), but the implementation deviates from the stated design and would need revisiting if a concurrent probe path is ever added.

- [S3] src/kernel/arch/x86_64/acpi.cpp:119-125 — File header comment claims the tables are "read in place without any page-table manipulation", contradicting the actual on-demand `VMM::map_page` implementation below it.
  WHY: Stale documentation — the code does map every accessed page on demand; the comment should say so (scope text and header disagree on the mechanism).

- [S3] src/kernel/iommu/iommu.cpp:271-305 — QI descriptors are written to the static `g_iq` WB-cache buffer with no cache flush (clflush/wbinvd) before the IOMMU is pointed at them.
  WHY: QEMU's memory model is coherent so the verified 5/5 run is unaffected; on real non-coherent hardware the IOMMU could read stale (zeroed) descriptors. Known QEMU-vs-real gap; note for the real-hardware follow-up.

- [S3] src/kernel/test/test_iommu_live.cpp:1228-1236 — `iommu_live_enable_sets_tes` and `iommu_live_fault_read_clear_safe` do not leave translation state clean: the final `force_present(false)` clears only software flags; GCMD.TE stays set in hardware for the remainder of the class.
  WHY: Contained (disk-free standalone class, iommu_live not in `all`, no kernel DMA after), but the "POST: none" annotation and "teardown disables" comment are inaccurate — there is no hardware TE teardown.

Verified non-issues (spot-checked against QEMU/Linux semantics): QI context-cache and IOTLB descriptors encode global granule correctly (`type|1<<4` = bits 4:5 = 01 per QEMU `VTD_INV_DESC_CC_G`/`VTD_INV_DESC_IOTLB_G`, reserved-field checks pass); IQT/IQH index-vs-byte-offset arithmetic matches QEMU; register offsets (VER 0x0 … IQA 0x90), GCMD/GSTS bit placement and RTADDR/IQA masks match the VT-d spec and QEMU; the lazy QIE enable is safe; the boot-time MMIO/ACPI page mappings are captured by the first snapshot and are never undone between iommu_live tests (`hhdm_modified_` stays false); no dynamic allocation on flush/RT paths (static queue + static tables); `iommu_live` is absent from `register_all_tests()` so `all` counts are unchanged; `force_present(false)` correctly makes `probe()==false` for cap_iommu determinism; no CONFIG_DEBUG-gated flow change (only `CONFIG_ARCH_X86_64` guards, symmetric); no IRQ-context path acquires `g_lock_` (plain busy-wait spinlock held during the bounded MMIO spins is consistent with pre-existing usage).

## PATCH
`audits/rejected_patch.diff` was written (applies cleanly with `git apply` on the current worktree). Intent: make the passthrough pre-pass actually skip devices already attached to a translation domain (never downgrade a T=1 entry to T=0), and keep `g_mmio_base` on the TE-timeout path so the cached re-probe can restore live authority and retry.

DECISION: REJECTED