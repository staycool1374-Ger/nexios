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
| IDT | IDT init/load (256 entries) | VBAR_EL1 init/load (64 slots) | stvec init/load (64 slots) |
| Controller | 8259A cascade (0x20/0x28), then APIC (fail-open to PIC) | GICv2/v3, Group 0, timer PPI INTID 30 | PLIC threshold 0, SEIE, prio-1 defaults |
| Controller ready | `is_apic_supported` + MMIO map | `gic_redist_ready()` (Children_Online; GICv2 = true) | n/a (no probe register) |
| Timer | PIT/APIC timer → TSC calibration | CNTP generic counter | SBI timer → mtime (10 MHz QEMU) |
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
- `mask`/`unmask` range guards: GIC `< 64` (snapshot covers words 0..1,
  IDT has 64 slots), PLIC `< 32` (single enable word on QEMU virt).
  Out-of-window lines are no-ops, never OOB MMIO writes.
- RISC-V `IrqState` additionally captures enable word 0; wired-line
  priorities default to 1 (lines stay disabled — drivers enable their
  own IRQ when registering handlers).
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
bring-up reuse.
