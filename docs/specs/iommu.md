# IOMMU DMA Protection (VT-d / AMD-Vi / SMMU) — Spec

**Status:** IMPLEMENTED (iteration-1, 2026-08-30, issue #4) — capability-gated
translation-table infrastructure; live DMAR register programming is an
explicit phase-2 sketch (§8).

**ROADMAP source:** v0.4.2 work item #4 ("IOMMU DMA protection (VT-d /
AMD-Vi / SMMU)") + Phase 4.6 aspirational "IOMMU DMA Protection Layer"
(tracking issue #9).

## 1. Goal & Threat Model

User-space drivers (Ring 3, Phase 4.6) must be unable to DMA-read or
DMA-write memory outside the frames they legitimately own. The IOMMU
translation layer enforces this at the HARDWARE interface: a device attached
to a protection domain can only reach frames explicitly mapped into that
domain — every other address faults. Authority is capability-gated end to
end:

```
IoMmuDmaCap (authority over ONE domain, single owner)
   + FrameCap (authority over specific frames, from the caller's CSpace)
        -> SYS_IOMMU_MAP / SYS_IOMMU_UNMAP -> IoMmuManager table programming
```

Kernel DMA (AHCI/virtio, `docs/specs/drivers.md` §2 contract) is NOT routed
through the IOMMU in iteration-1 (identity/passthrough until the kernel-DMA
domain lands in phase-2).

## 2. Components (verified current state)

| Component | File | Notes |
|---|---|---|
| `CapType::IoMmuDma` | `src/kernel/cap/cap_types.hpp` | type 8 |
| `IoMmuDmaCap : KernelObject` | `src/kernel/cap/iommu.{hpp,cpp}` | `CONFIG_CAP_MAX_IOMMU`; `create()` = probe gate + domain_create + MemPool; `revoke()` destroys the domain FIRST (fail-closed) |
| VT-d entry layouts | `src/kernel/iommu/vtd.hpp` | SL R/W/E bits 0-2, addr bits 51:12; root P bit 0 + CTP; CTE P/FPD/T(bits 3:2, T=1 translate)/ASR bits 63:12 — single source of truth |
| `IoMmuManager` | `src/kernel/iommu/iommu.{hpp,cpp}` | static bounded tables, one leaf SpinLock, identity-IOVA 4-level SL walk, cascade-empty-free |
| Handlers | `src/kernel/syscall/syscall_handlers_iommu.cpp` | `SYS_IOMMU_MAP`(59) / `SYS_IOMMU_UNMAP`(60), `MAX_SYSCALL=61` |
| Tests | `src/kernel/test/test_cap_iommu.cpp` | class `cap_iommu`, 12 tests |
| Tunables | `src/kernel/nexios_config.h` | `CONFIG_CAP_MAX_IOMMU`(16), `CONFIG_IOMMU_MAX_DOMAINS`(8), `CONFIG_IOMMU_MAX_MAPPINGS`(32), `CONFIG_IOMMU_MAX_BUSES`(8) |

## 3. Domain Model

- **Domain** = one private second-level (SL) page-table root (one PMM page)
  + a bounded mapping-record table (`CONFIG_IOMMU_MAX_MAPPINGS`) + the
  owning task id. Domains live in a static table
  (`CONFIG_IOMMU_MAX_DOMAINS`); no dynamic allocation beyond SL pages.
- **Identity IOVA:** the DMA address of a mapped frame IS its physical
  address (`iova == phys`). A device can only reach mapped frames; every
  unmapped address faults once translation is attached (T=1).
- **Mapping** = contiguous frame range `[phys, phys + pages*4KiB)` with SL
  flags (R|W) derived from the FRAME SLOT's granted rights. Overlapping
  mappings are rejected (one owner per IOVA range).
- **Device attach** = root entry (bus) Present + context-table pointer;
  context entry (device*8+function) Present + T=1 + ASR = domain root.
  Static per-bus context tables (`CONFIG_IOMMU_MAX_BUSES`), one root table
  (256 x 16 B = exactly one 4 KiB page). Re-attach re-points the device
  (one device → one domain; last attach wins). `clear_attachment` zeroes
  both entries.

## 4. Authority & Validation Chain (binding)

`sys_iommu_map(dma_handle, frame_handle)`:

1. `IoMmuManager::probe()` — absent IOMMU → -1 (graceful degradation; kernel
   DMA untouched).
2. `cap::lookup(cspace, dma_handle, IoMmuDma, CAP_RIGHT_WRITE)` — WRITE is
   the arming authority (issue #2/#3 right-split pattern).
3. `cap::lookup(cspace, frame_handle, Frame, 0)` — frame from the caller's
   own CSpace.
4. Owner check: `IoMmuDmaCap::owner_task_id_ == current task id` (strict
   single-owner; a granted/minted cap held by another task is REFUSED).
5. Slot re-read: the flags come from the EXACT slot the handle addresses
   (type + object identity + generation re-validated); READ→R, WRITE→W.
6. `map_frame` re-validates domain liveness, alignment, non-empty range,
   revoked frame, overlap, record capacity — every failure returns -1 with
   NO table mutation.

`SYS_IOMMU_UNMAP` uses the identical chain (unmap of a revoked frame stays
allowed — authority loss must not block teardown; `domain_destroy` never
needs a FrameCap).

## 5. Memory Discipline (binding)

- **Zeroing:** PMM pages are NOT zeroed — every SL table page is memset(0)
  at allocation (`alloc_zeroed_table_page`). A stale R/W entry would grant
  DMA to recycled frames (S1).
- **Single-owner pages:** every SL page belongs to exactly one domain and is
  freed exactly once — cascade-free during unmap/rollback or
  `domain_destroy` walking only still-occupied records. A cleared record is
  never walked again.
- **Rollback:** a mid-map failure (PMM exhaustion, table walk failure) clears
  every entry written by THIS call and cascade-frees the now-empty tables —
  no partial mapping is ever visible; shared tables survive (other
  mappings keep non-zero entries).
- **Cascade-free:** after a leaf entry is cleared, each ancestor table is
  freed only when all 512 entries are zero (bounded 512-scan per level);
  the root is freed only by `domain_destroy`.
- **ResourceTracker:** SL pages fold into `pmm_pages_used`; caps fold into
  `cap_objects`/`cap_slots`. Every test ends with zero deltas and an EMPTY
  domain table (a stale root would dangle after the PMM snapshot rewind).

## 6. Concurrency Boundaries

- One leaf `SpinLock` serializes ALL manager state. It is a LEAF lock:
  never held while calling into cap/cspace layers, `dispose()`,
  `MemPool::free`; never held across `Scheduler::reschedule()`.
- Syscall pin order: `cap::lookup` (cspace lock, brief, released inside
  lookup) → manager lock (brief) → release pins AFTER the manager call
  returns. No nested cspace+manager lock scope ever overlaps.
- Presence flag (`probe()`/`force_present()`) is a plain bool; control flow
  is identical in debug and release (no `#ifdef` divergence — audit rule).

## 7. SIL 3 Analysis

| Risk | Class | Mitigation |
|---|---|---|
| Unzeroed table page grants DMA to recycled frames | S1 | memset(0) at every table-page alloc; tests 6/7 assert clearing + page frees |
| SL page double-free (unmap vs destroy overlap) | S1 | single-owner discipline; destroy walks only occupied records; exhaustion test asserts zero PMM delta |
| Revoke leaves DMA access | S1 | `revoke()` destroys the domain BEFORE publishing the revoke |
| Cap-gating bypass (foreign frames / foreign owner) | S1 | rights + type + generation + owner-task + domain-liveness all validated; any failure → -1, no mutation (test 10 matrix) |
| VT-d bit infidelity | S2 | single header (vtd.hpp); tests assert field placement by direct table inspection |
| Syscall table off-by-one | S2 | explicit constexpr table, compile-time sized (MAX_SYSCALL=61), build-verified |
| IOVA bypass | S2 | T=1 (translate) for all user domains; passthrough reserved for kernel-owned BDFs in phase-2 only |
| Stale domain after test leak | S3 | tests assert `occupied_domains()==0`; ResourceTracker check at snapshot_restore |
| probe() is a stub flag | S3 | live detection + DMAR programming documented phase-2 (§8) — no false sense of live protection |

## 8. Phase-2 Sketch (NOT implemented — live VT-d)

1. ACPI/DMAR table discovery (repo has no ACPI parser today) → DRHD units.
2. `probe()` becomes real (DMAR register presence + CAP reg).
3. Enable translation: GCMD.TES with IRTA/RTA programming + IOTLB flush
   (register-level invalidation on every map/unmap).
4. Fault-event handling (FRI → fault log → task kill, fail-closed).
5. Kernel-DMA domain (AHCI/virtio buffers mapped instead of passthrough).
6. QEMU integration: `-device intel-iommu` boot + live fault tests.
7. AMD-Vi (IIOMMU) / SMMUv3 (aarch64) backends behind the same
   `IoMmuManager` interface — the table model is deliberately VT-d-shaped
   but the cap/syscall layer is backend-neutral.

## 9. Test Plan (implemented, class `cap_iommu`, 12)

probe-false graceful degradation / cap lifecycle + revoke fail-closed /
SL entry programming (2-page walk) / rights flow R|W / revoked+unaligned
rejection / unmap clears + reclaims L1 page / destroy frees all / device
attach context entry / syscall happy dispatch / syscall validation matrix
(absent IOMMU, bad handle, wrong type, missing WRITE, stale gen, foreign
owner) / unmap dispatch (happy + already-unmapped + bad handle) / bounded
exhaustion (domains + mappings) fail-closed.
