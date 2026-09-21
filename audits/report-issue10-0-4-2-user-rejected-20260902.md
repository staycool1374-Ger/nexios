# SIL 3 Audit Report — Issue #10 (MSI-X Vector Infrastructure)

- **Auditor:** SIL 3 auditor subagent (read-only; no files modified, no tests run)
- **Date:** 2026-09-02T07:22:14Z
- **Patch reviewed:** `audits/pending_patch.diff` (against `main`)
- **Files touched (18):**
  - `src/kernel/arch/pci.hpp` + `src/kernel/arch/pci.cpp` — `MsixTableEntry`/`PciMsixTableInfo`, `pci_msix_table_info`, `pci_program_msix_entry`, `pci_msix_entry_set_masked`/`_masked`, boot-time `pci_msix_premap_all` + cached table mapping, rewritten `pci_enable_msix` (KVA-mapped programming, no raw physical `reinterpret_cast`), hardened `pci_find_capability` (phantom-device reject + bounded 256-step walk)
  - `src/kernel/cap/msix.hpp` + `msix.cpp` (NEW) — `MsixCap` per-(BDF,entry) single-owner claim registry, vector alloc + `claim_slot` under one `IrqGuard`, masked-at-create programming, dispose/revoke teardown, `snapshot_reset`
  - `src/kernel/irq_delivery.hpp` + `.cpp` — `IrqSlotKind{PIC,MSIX}`, owner widened to `KernelObject*`, window widened to 33–255 (rejects `<33`, `==0x80`, `==TIMER`, threaded, occupied), kind-gated PIC/MSI-X mask hooks, `arm()` gains a vector param
  - `src/kernel/syscall/syscall_handlers_irq.cpp` — dual `CapType::Irq|Msix` lookup, `arm` vector param
  - `src/kernel/cap/cap_types.hpp` (`CapType::Msix=9`), `src/kernel/cap/irq.cpp` (PIC window 33–47), `src/kernel/nexios_config.h` (`CONFIG_CAP_MAX_MSIX=8`)
  - Tests: `test_cap_msix.cpp` (NEW, 12), `test_cap_irq.cpp`, `test_cap_irq_notify.cpp`, `test_isolate.cpp`, `test_registry.cpp`, `test_expected_counts.hpp`
  - Docs: `docs/specs/cspace.md`, `docs/specs/configuration.md`

**Rules applied:** `prompts/CODING_STYLE.md` §5 (error handling / ENSURE vs error), §6 (bounded loops, no primitive reinterpret_cast to physical addresses), §10 (kernel rules), §11 (concurrency, timer-vector single-owner), §12 (lifecycles, wakers-own-wakeup, ownership checks). Issue #2 discipline (slot reuse: owner+vector revalidated in ONE atomic section; stale `reg_idx_` must never arm/release a foreign drained-and-reused slot), issue #10 fail-closed MSI-X mask contract.

---

## FINDINGS

### S1 — `src/kernel/irq_delivery.cpp:109-120` + `src/kernel/arch/pci.cpp:532-537,540-551` — CODING_STYLE §11 timer-vector single-owner invariant — the widened MSI-X window (48–255) does not exclude the two kernel-reserved vectors inside it: vector **64** (xAPIC timer, `arch::APIC::APIC_TIMER_VECTOR`; `CONFIG_USE_APIC_TIMER=1` default and `timer.cpp:46-57` register the scheduler tick handler there) and vector **255** (APIC spurious, `arch::APIC::SPURIOUS_VECTOR`, programmed at `apic.cpp:112`).

`pci_alloc_vector()` (pci.cpp:542 `for v=48..255`) never reserves 64 or 255, and `claim_slot()` rejects only `<33`, `0x80`, `==TIMER(32)`, threaded and occupied — so `MsixCap::create` can legitimately obtain and claim vector 64. When that slot is armed via `sys_irq_register`, `handle_interrupt_c` (kernel.cpp:1603) calls `IrqDelivery::isr_entry(64)` on every scheduler tick: the armed slot consumes the tick, EOI-acks, wakes the user waiter, and returns **early** — `IDT::handle_interrupt` → `Scheduler::on_tick()` never runs. This is total scheduler starvation (no preemption, no deadline checks, watchdog failures) caused by the exact class of vector the existing code already protects for the PIC timer (vector 32). The `claim_slot` window widening to 48–255 must have extended the reserved-vector rejection list; it did not.

### S3 — `src/kernel/arch/pci.cpp:798-803` (`pci_enable_msix`) — behavioral contract — the rewritten legacy path leaves the table entry MASKED (`pci_program_msix_entry` writes `MSIX_ENTRY_MASK_BIT`), but this raw kernel path has no arm step, so a legacy caller receives a vector that never delivers; no production caller exists today (only the no-cap test at `test_pci.cpp:198`), so this is a latent dead-code contract break, not a live regression — unmask in the legacy path to restore the "live vector" contract (included in corrective patch).

### S3 — `src/kernel/arch/pci.cpp:655-665` (`msix_cache_store`) — idempotency — the store does not dedupe by BDF, so every repeated `pci_scan_all()` (boot, `virtio_pci.cpp:93`, and every test re-probe at `test_cap_msix.cpp:59`/`test_pci.cpp`) appends a duplicate cache entry for the same device (bounded by 128, so no overflow); `msix_cache_lookup` returns the first match so correctness is unaffected — dedupe added in corrective patch.

### S3 — `src/kernel/arch/pci.cpp:723-727` (`pci_msix_table_info`) + `src/kernel/arch/pci.cpp:567,700,739` — §5.1 / fail-closed — (a) `VMM::map_page` is `void` and its OOM failure (PT-page alloc fail inside `get_table`) is ignored: on failure `out.table_kva` is still reported mapped and a later `pci_program_msix_entry` MMIO write would #PF (panic on a reachable-exhaustion path; matches the iommu/virtio_pci precedent and is boot-time/low-risk, hence S3); (b) the `bdf.bus >= PCI_MAX_BUSES` (256) comparisons are dead checks because `PciBdf::bus` is `uint8_t` (always < 256) — harmless, the phantom-device `pci_device_exists` probe fails closed instead. No code change required for (b).

### Verified clean (no finding)
- **Rollback paths** (`msix.cpp:90-181`): every `create()` failure path (vector alloc fail, `claim_slot` fail, `MemPool` fail, `pci_program_msix_entry` fail, capability-not-found) releases vector + slot + entry claim exactly once; `release_slot_idx(reg_idx, nullptr)` only ever sees `armed==false` (a slot cannot arm before `set_slot_owner`), so the MSI-X `r->owner` cast in `release_slot_idx` (irq_delivery.cpp:283) can never hit a null owner. Entry-claim registry (msix.cpp:61-87) check-then-set is non-yielding, so cross-task preemption can only cause a correct fail-closed collision, never a double-claim.
- **Slot-reuse safety** (irq_delivery.cpp:170-172, 264-276, 318-364): `arm`/`release_slot_idx`/`drain_task` revalidate `occupied` + `owner` (+ `vector` in `arm`) under the per-slot lock in one atomic section; stale `reg_idx_` cannot arm/release a foreign drained-and-reused slot. `drain_task` runs BEFORE `release_all_objects()` in `TaskControlBlock::cleanup()` (task.cpp:1884 vs 1886), so `r->owner` (the `MsixCap`) is still alive when the MSI-X entry is re-masked — no use-after-free.
- **Kind-gated masking** (irq_delivery.cpp:78-90, 183-201, 282-285, 348-352): PIC `vector-32` line arithmetic and MSI-X entry re-mask are each gated on `IrqSlotKind`; the entry is re-masked on every release path (dispose msix.cpp:199, revoke msix.cpp:214, `release_slot_idx` irq_delivery.cpp:282, `drain_task` irq_delivery.cpp:348). `IrqCap::create` (irq.cpp:47) enforces its own 33–47 PIC window so an `IrqCap` can never own a MSIX-kind slot (and vice versa) — the `static_cast<cap::MsixCap*>` in `arm`/release is type-safe by construction.
- **Bounds** (msix.cpp:92-94,105; pci.cpp:567-571,700-717,739-743): BDF device/function bounds effective; `entry_index < entry_count`; table fits the memory BAR; page-floor/page-ceil mapping (pci.cpp:723-727) means no entry straddles an unmapped page.
- **RT discipline** (pci.cpp:65 static table; msix.cpp:57 static registry): no dynamic allocation on RT paths; no spinlock held across a reschedule (all slot-lock critical sections are leaf operations); no ENSURE on reachable conditions (exhaustion returns error codes).
- **ResourceTracker** (msix.cpp:179,208): `track_cap_object_add`/`remove` paired with `create`/`dispose`, `revoke` correctly untouched (matches IrqCap/mmio pattern); `MsixCap::snapshot_reset` wired into `snapshot_restore` (test_isolate.cpp:1360) with correct placement.
- **Syscall handlers** (syscall_handlers_irq.cpp:54-137,146-263): dual `Irq|Msix` lookup without RTTI (type remembered in `is_msix`, correct static cast); every path fails closed with -1 and releases the acquired object exactly once; raw 64-bit `arg1` validated (`!=0 && !=1`) before the narrowing cast, defeating the 0x101/0x100 wrap attack.

---

## Corrective patch

`audits/rejected_patch.diff` — `git apply`-able against the worktree (verified with `git apply --check`; offsets OK).

Fixes every S1/S2 finding plus the S3 hardening:

1. **S1 — reserve kernel vectors 64/255** in `pci.cpp` `init_vector_alloc()` (so `pci_alloc_vector` never hands out the xAPIC scheduler timer or the APIC spurious vector).
2. **S1 — reject kernel vectors 64/255** in `claim_slot()` (`irq_delivery.cpp`), gated `CONFIG_ARCH_X86_64`, referencing `arch::APIC::APIC_TIMER_VECTOR` / `arch::APIC::SPURIOUS_VECTOR` (defense in depth: even a hand-crafted vector not sourced from `pci_alloc_vector` cannot claim a kernel-reserved slot).
3. **S3 — restore legacy `pci_enable_msix` live-vector contract** (unmask the entry in the raw path).
4. **S3 — dedupe `msix_cache_store` by BDF** (repeated `pci_scan_all` no longer grows duplicate cache entries).

Note on fix #1/#2: reserving vector 64 unconditionally costs one MSI-X vector even when `CONFIG_USE_APIC_TIMER=0` (PIT mode) — an acceptable conservative choice; the alternative (conditional reservation on the APIC-active boot path) is more complex and not justified.

---

DECISION: REJECTED