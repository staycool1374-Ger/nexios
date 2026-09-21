# SIL 3 Audit Report — Issue #10 (MSI-X Vector Infrastructure) — RE-AUDIT (iter-2)

- **Auditor:** SIL 3 auditor subagent (read-only; no files modified, no tests run)
- **Date:** 2026-09-02T07:28:38Z
- **Patch reviewed:** `audits/pending_patch.diff` (cumulative, against `main`, 18 files) — supersedes the iter-1 patch and its `audits/report-2026-09-02T07-22-14Z.md` report
- **Scope of this re-audit:** verify the iter-1 S1 fix (kernel-reserved vectors 64/0xFF) is complete and correct; confirm the new test exercises the rejection; check for regressions introduced by the fix; re-confirm the S3 hardening.
- **Rules applied:** `prompts/CODING_STYLE.md` §5 (fail closed / no ENSURE on reachable paths), §6 (bounded loops), §11 (timer-vector single-owner), §12 (lifecycle / ownership revalidation). Issue #2 discipline (slot-reuse: owner+vector revalidated in one atomic section). Issue #10 MSI-X fail-closed mask contract.

---

## Re-audit of the iter-1 S1 fix (task item 2)

### 1. Are 64 and 0xFF rejected in BOTH `pci_alloc_vector` AND `claim_slot`?

**Yes — confirmed in the worktree (read directly, not just the diff):**

- **`pci_alloc_vector`** — `src/kernel/arch/pci.cpp:537-538`:
  ```cpp
  g_vector_used[64] = true;    // xAPIC scheduler timer (APIC_TIMER_VECTOR)
  g_vector_used[0xFF] = true;  // APIC spurious interrupt vector
  ```
  `pci_alloc_vector()` (pci.cpp:542-553) iterates `48..255`, skips `0x80`, and returns the first `!g_vector_used[v]`. With 64 and 0xFF pre-marked `true` in `init_vector_alloc()`, the allocator can never hand them out.

- **`claim_slot`** — `src/kernel/irq_delivery.cpp:118-126`:
  ```cpp
  #if defined(CONFIG_ARCH_X86_64)
      if (vector == static_cast<uint8_t>(arch::APIC::APIC_TIMER_VECTOR) ||
          vector == 0xFF)
          return -1;
  #endif
  ```
  Confirmed present and gated `CONFIG_ARCH_X86_64`, referencing `arch::APIC::APIC_TIMER_VECTOR` (64). Defense in depth: even a hand-crafted vector not sourced from `pci_alloc_vector` cannot claim a kernel-reserved slot.

### 2. Any OTHER kernel-reserved vector inside 48–255 still admitted?

**No.** Exhaustive check of vector-holder sites:

- The **only** `IDT::register_handler_raw` call in the kernel is `src/kernel/arch/x86_64/hal/timer.cpp:50` → `APIC::APIC_TIMER_VECTOR` (64). Now rejected in both layers.
- The **APIC spurious vector** (0xFF) is programmed only into the SVR (`apic.cpp:112`, `REG_SPURIOUS`); no ISR handler is registered at 0xFF, but it is still rejected in `claim_slot` and reserved in the allocator (0xFF is the xAPIC spurious vector and must never be a user-deliverable MSI-X vector). Correct to exclude.
- The **APIC error/thermal/perfmon LVT entries** are all written MASKED (`apic.cpp:115-120`) and no handler is registered at their vectors — they are inactive, so they are not claimable/allocatable sources and require no reservation. (Even if later activated, they would follow the same `claim_slot` rejection discipline.)
- The **threaded-IRQ** vector (keyboard, `IrqThread::create(33,…)`, kernel.cpp:342) is vector 33 — inside the PIC window 33–47, already rejected by `claim_slot` via `IrqThread::for_vector()` (irq_delivery.cpp:128). No conflict with the 48–255 MSI-X window.
- Legacy I/O-APIC routing (`apic.cpp:130-133`) maps IRQ0–15 → vectors 32–47 only; vectors ≥48 are exclusively MSI/MSI-X, i.e. exactly the `pci_alloc_vector` domain.

**Conclusion:** 64 and 0xFF are the only kernel-reserved vectors inside 48–255, and both are now rejected in `pci_alloc_vector` AND `claim_slot`. The S1 finding is fully remediated.

### 3. Does the new test actually exercise the rejection?

**Yes.** `src/kernel/test/test_cap_msix.cpp:1496-1504` (`msix_kernel_reserved_vectors_rejected`, registered at line 2144):
```cpp
JARVIS_ASSERT(IrqDelivery::claim_slot(64) < 0);    // xAPIC timer rejected
JARVIS_ASSERT(IrqDelivery::claim_slot(0xFF) < 0);  // APIC spurious rejected
int16_t idx = IrqDelivery::claim_slot(48);
JARVIS_ASSERT(idx >= 0);                           // 48 still accepted
JARVIS_ASSERT(IrqDelivery::release_slot_idx(idx, nullptr));
JARVIS_ASSERT_EQ(static_cast<size_t>(0), IrqDelivery::occupied_count());
```
It calls `claim_slot(64)` and `claim_slot(0xFF)` directly and asserts both reject, plus a positive control (`48` accepted and released) and a ResourceTracker/occupancy-clean teardown. It is gated `CONFIG_ARCH_X86_64` (both in the test body and in the registration), so it only runs where the `claim_slot` rejection is compiled in. The positive control confirms the rejection is specific (not an over-broad "everything fails") — the test would catch a regression that accidentally reserved vector 48 too.

**Expected-count bookkeeping** is consistent: `test_expected_counts.hpp` bumps `cap_msix` 12→13 (line 2174) and `all` 1016→1029 (line 2166), matching the added test.

### 4. Regression check — did reserving 64/0xFF break anything?

- **No double-counting / allocator corruption:** `init_vector_alloc()` sets `g_vector_used[64]`/`[0xFF]` once and is guarded by `g_vector_init` (pci.cpp:529-540) — the same mechanism already reserves 0–47 and 0x80. `pci_free_vector` (pci.cpp:555-559) rejects `vec < 48` and `vec == 0x80` but would clear 64/0xFF if ever called with them — however the allocator never returns them, and no caller frees a non-allocated vector. No new invariant is violated.
- **IRQ-cap / PIC tests:** `IrqCap::create` (irq.cpp) enforces its own 33–47 window; vector 64/0xFF were never in the PIC test set. `test_cap_irq.cpp:636` and `test_cap_irq_notify.cpp:540` were only updated for the new `arm()` vector parameter — the underlying PIC vectors (kTestVector in 33–47) are unaffected by the 64/0xFF reservation.
- **`pci_alloc_free_vector` (test_pci.cpp:166-177):** asserts `v1 >= 48 && v1 != 0x80`. The allocator now returns 48 then 49 (both un-reserved), so the test still passes — no expectation pinned to vector 64/255.
- **MSI-X tests:** `msix_cap_create_fields` (test_cap_msix.cpp:1391) asserts `msix->vector >= 48`, no specific vector pinned. With 64/0xFF reserved, the allocator yields 48 for the first device entry — valid, masked, and released cleanly.
- **Reserving 64 unconditionally when `CONFIG_USE_APIC_TIMER=0`** (PIT mode) costs exactly one MSI-X vector. The task brief explicitly flags this as acceptable (conservative; same reasoning as the iter-1 report). The default and the enforced hard-RT config (`test_config_checks.cpp:103`) require `CONFIG_USE_APIC_TIMER=1`, so in practice vector 64 is always the active scheduler tick anyway — the reservation is not just conservative, it is correct for every supported configuration.

### 5. S3 items from iter-1 — still remediated (re-confirmed in the patch)

- **Legacy `pci_enable_msix` live-vector contract** — `pci.cpp:348` calls `pci_msix_entry_set_masked(tbl, entry, false)` after programming the entry and enabling the function, restoring the "hands back a live vector" contract for the raw (non-cap) path.
- **`msix_cache_store` BDF dedupe** — `pci.cpp:150-157` now returns early on a duplicate BDF before appending, so repeated `pci_scan_all()` cannot grow duplicate cache entries.

These were S3 (non-blocking) in iter-1 and are correctly resolved.

## Prior audit context (re-confirmed clean, not regressed by the S1 fix)

The iter-1 "verified clean" items were re-checked against the applied worktree (all present and unchanged in substance):
- create() rollback paths release vector + slot + entry claim exactly once (`msix.cpp`)
- arm / release_slot_idx / drain_task revalidate owner+vector under the slot lock, one atomic section
- kind-gated PIC/MSI-X masking on every release path; `IrqCap::create` enforces its own 33–47 window
- BDF/entry/table bounds; page-floor/page-ceil mapping; no RT-path allocation
- ResourceTracker pairing + `snapshot_reset`; dual-lookup syscalls (no RTTI, fail-closed -1, arg1 wrap rejected)

---

## FINDINGS

No S1, S2, or S3 findings remain.

- *(none)* — reserved-vector S1: **fixed** — 64 (`APIC_TIMER_VECTOR`) and 0xFF (`SPURIOUS_VECTOR`) are now reserved in `init_vector_alloc` (`pci.cpp:537-538`) and rejected in `claim_slot` (`irq_delivery.cpp:118-126`); the only other raw ISR vector in 48–255 (64) is covered, and no other active kernel vector occupies the window.
- *(none)* — test coverage: **fixed** — `msix_kernel_reserved_vectors_rejected` (`test_cap_msix.cpp:1496-1504`) directly asserts `claim_slot(64)` and `claim_slot(0xFF)` reject, with a positive control and occupancy-clean teardown; expected-count tables updated (13 / 1029).
- *(none)* — regression: **none introduced** — reserving 64/0xFF does not double-count, does not break PIC/IRQ-cap tests, does not break `pci_alloc_free_vector` or the MSI-X field assertions, and is correct for every supported (APIC-timer) configuration.
- *(none)* — iter-1 S3 items: **resolved** — legacy `pci_enable_msix` unmask (`pci.cpp:348`) and `msix_cache_store` BDF dedupe (`pci.cpp:150-157`).

---

DECISION: APPROVED
