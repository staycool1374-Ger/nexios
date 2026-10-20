# Early IRQ-Controller + Timer Init (issue #198)

Single per-arch entry point run before the scheduler exists. Fail-closed:
reachable failures are `EarlyIrqResult` codes, never panic.

## 1. Ordering contract

```
IDT::init + IDT::load
  → ArchInterruptController::init
    → Timer::init(frequency_hz)
      → Timer::calibrate
```

Global IRQs stay masked throughout. `early_irq_init()` is idempotent
(BOOT_ONLY): a repeat call re-runs the sequence with no hardware
side-effect delta and returns the same result.

Success criterion is `Timer::freq_hz() != 0` after calibrate — not the
calibrate boolean (x86 calibrate returns false on the nonzero fallback
source, which is a success).

## 2. Per-arch mapping

| Stage | x86_64 | AArch64 | RISC-V |
|---|---|---|---|
| IDT | IDT init/load (256 entries) | VBAR_EL1 init/load (256 slots) | stvec init/load (256 slots) |
| Controller | 8259A cascade (0x20/0x28), then APIC (fail-open to PIC) | GICv2 full init (CTLR, 8 enable banks, IPRIORITY, ITARGETSR=CPU0, ICFGR, GICC PMR, timer PPI INTID 30; v3 path coherent) | PLIC S-context 1 (threshold 0, priorities 1, 3 enable words, SEIE) |
| Controller ready | `is_apic_supported` + MMIO map | `gic_redist_ready()` (v2: CTLR read-back; v3: Children_Online + CTLR) | n/a (no probe register) |
| Timer | PIT/APIC timer → TSC calibration | CNTP generic timer, CVAL absolute deadlines | SBI timer → mtime (10 MHz QEMU), absolute deadlines |
| AP path | `early_irq_init_ap()`: `APIC::init_ap` + `IDT::load` only | per-core GICR (issue #28) | per-hart PLIC threshold (issue #29) |

## 3. Error codes

`OK`, `BAD_FREQ` (freq 0), `MMIO_MAP_FAILED` (x86 APIC window),
`CTRL_PROBE_FAILED` (reserved: unreadable version), `CTRL_TIMEOUT`
(GIC redistributor WAKER bound expiry — GIC left disabled = safe state),
`TIMER_FREQ_UNKNOWN` (backing frequency still 0).

## 4. Hardening landed with this contract

- GIC redistributor WAKER poll is bounded by `GICR_WAKER_MAX_POLLS` with
  spec-correct Children_Online polarity (pre-#198 loop tested the
  inverted bit and always spun the full budget on QEMU). On expiry
  `gic_redist_ready()` is false and `early_irq_init` returns CTRL_TIMEOUT
  without starting the timer (the distributor itself was already enabled
  by `init()` — the error stops the bring-up sequence, it does not unwind
  the controller).
- `mask`/`unmask` range guards: GIC 0–255 (8 enable banks),
  PLIC 0–95 (3 S-context enable words). Out-of-window lines are no-ops,
  never OOB MMIO writes. GIC uses write-1 set/clear registers (no lock);
  PLIC enable-word RMW runs under `arch::IrqGuard` (thread vs ISR storm
  interleaving).
- RISC-V `IrqState` captures threshold + all three S-context enable
  words; wired-line priorities default to 1 (lines stay disabled —
  drivers enable their own IRQ when registering handlers).
- AArch64 `IrqState` captures all 8 GICD enable banks (`gic_mask` keeps
  banks 0–1 for compatibility); restore never touches CTLR, priorities,
  targets, or ICFGR.
- GIC version comes from GICD_PIDR2[7:4] (2 = v2, fail-safe), never
  from GICD_TYPER[15:11] (not a version field). Re-init is a BOOT_ONLY
  no-op (latch); readiness is proven by CTLR read-back, not assumed.
- AArch64 `oneshot`/`periodic` take coarse scheduler ticks at
  `BootParams::timer_hz` (fallback `CONFIG_TICK_HZ`), arm absolute
  CVAL deadlines with saturating math, and latch the periodic interval
  for the ISR re-arm. `remaining()` returns ceil coarse ticks, gated on
  CTL.ENABLE (a TVAL read is `cval - count`, not a stored zero);
  `remaining_ns()` derives from the same clamped counter remainder.
- RISC-V `set_frequency` arms `now + interval` (the old code passed
  the bare interval as an absolute deadline, firing immediately).
  `oneshot`/`periodic` convert coarse ticks the same way and latch
  the periodic interval; `remaining*` stay hard 0 by S-mode hardware
  constraint (mtimecmp unreadable).
- EOI ownership: the ack path writes EOIR/ICC_EOIR1 (`eoi()` only for
  the last-acked INTID on aarch64); `handle_plic_trap` pairs every
  claim with exactly one complete, and the S-mode `eoi()` is a
  documented no-op. Spurious vectors (>= 1020 GIC / claim 0 PLIC)
  are counted, never EOI'd/completed, never dispatched.
- Storm limiter (`IrqStormLimiter`, `CONFIG_IRQ_STORM_LIMIT` = 256
  same-INTID dispatches per tick): masks the line + one dmesg ERROR
  (`ErrorSubsystem::BASE` + 64). The timer INTID is never fed.
- Zero-frequency calls are fail-closed on all arches (`set_frequency(0)`
  and `early_irq_init(0)` program no hardware).
- RISC-V `handle_irq` re-arms relative (`now + timer_interval_`, latched
  at `set_frequency`/`periodic`) instead of the drifting absolute
  `ticks_ * interval` product; `oneshot(0)`/`periodic(0)` disarm
  (mtimecmp parked at maximum — S-mode cannot clear a pending STIP).
- AArch64 `oneshot(0)`/`periodic(0)` disarm via CTL disable (was a
  divide-by-zero on `period_ticks == 0`).

## 5. Non-goals

Full production boot paths (#28 aarch64, #29 riscv64), DTB plumbing
(#183), teardown validation (#199). `kernel.cpp` keeps its own staged
calls; `early_irq_init()` codifies the same order for tests and
bring-up reuse. Out of scope for #349: x86-only remainders (GDT/IDT
gates, PIC, APIC/LAPIC/IOAPIC, HPET, MSI/MSI-X, SMP/IPI),
interrupt-driven UART RX (#245/#247), SMP per-hart contexts (#351),
GICv3 HW validation (QEMU cortex-a72 exercises GICv2 only), riscv
release (#207), TCG wall accuracy (#359/#384), QE test
authorship/counts/release-subset. Virtio-MMIO transport/IRQ consumers
landed in #350 (drivers keep the poll fallback fail-closed).

## 6. Per-arch IRQ routing + timer parity (issue #349)

QEMU-virt INTID map (aarch64): UART0 = 33, RTC = 34, PCIe INTx =
35–38, virtio-mmio = 48–79 (2 MiB Device leaf at PA `0x0A000000`, L2
index 80). QEMU-virt PLIC map (riscv64, S-mode context 1): virtio-mmio
= 1–8, UART = 10, RTC = 11, PCIe INTx = 32–35; S-context addresses are
`0x0C002080` (enable), `0x0C201000` (threshold), `+4` (claim/complete).
Virtio-mmio transport consumers (issue #350) register raw IDT handlers
at INTID 48 + slot (GIC) / IRQ 1 + slot (PLIC), unmask the source, and
ACK `InterruptStatus` after draining (drain-before-ACK, bounded
re-check); teardown masks first.

GICv2 programming order: CTLR=0 → disable all banks → IGROUPR=0
(Group 0) → IPRIORITY = 0xA0 → ITARGETSR = CPU0 for SPIs (reset
targets are not CPU0) → ICFGR level, edge only for virtio-mmio
(QEMU virt DT flags it edge-high) → timer PPI enable → CTLR=1 →
GICC_CTLR=1 with read-back. Version from PIDR2, clamped window
`min(256, (ITLinesNumber+1)*32)`.

Timer units contract: `oneshot`/`periodic` take coarse scheduler
ticks; `remaining()` returns ceil coarse ticks (0 disarmed/expired);
`remaining_ns()` the sub-tick remainder. riscv64 `remaining*` are hard
0 (S-mode cannot read mtimecmp) — enforcement tests must assert the
contract, never wall time.
