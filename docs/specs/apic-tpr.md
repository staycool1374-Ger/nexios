# APIC TPR-Based Interrupt Prioritization & Core-State Isolation

**Doc ID:** NEX-SPEC-2026-09-14-001
**Status:** APPROVED (SIL 3 spec audit `audits/report-2026-09-14T15-30-35Z.md`,
2×S2 + 4×S3 found and fixed in-spec)
**Milestone target:** v0.4.4 — issue #26
**Related:** `docs/specs/per-cpu-smp.md` §3.4 (C1 scope — untouched except
a pointer), issue #25 (per-CPU/SMP), issue #27 (per-CPU asm checker INV-PC2).

## 1. Current State (verified 2026-09-14)

- `REG_TPR = 0x080`, `REG_PPR = 0x0A0` exist
  (`src/kernel/arch/x86_64/hal/apic.hpp:103-105`) but no code reads them.
- `APIC::enable_local()` (`apic.cpp:74`) programs TPR = 0 (accept-all) via
  function-local `wr`/`rd` lambdas (mode-gated x2APIC-MS R/`x2apic_msr(off)`
  = `0x800 + (off >> 4)` — hence TPR = MSR **0x808**, not 0x880 — else xAPIC
  MMIO). No other TPR touch exists in the tree.
- Tick source: `CONFIG_USE_APIC_TIMER = 1` + APIC enabled ⇒ the **APIC timer
  drives the system tick** (`timer.cpp:51`); the PIT is not programmed.
  `APIC_TIMER_VECTOR = 64` (priority class 0x4), `SCHED_VECTOR = 0xEC`
  (class 0xE), syscall `0x80` (trap), spurious `0xFF`.
- All non-timer LVTs are masked at init (`apic.cpp:88-92`); only the timer
  LVT is live. `isr_nesting_depth` is per-CPU at `gs:0x20` (#27).
- `PerCpu` has `reserved[475]` free (`percpu.hpp:74`); next slot is `gs:0x40`.

## 2. Problem

Raising TPR is the only way to mask a *priority band* of external
interrupts without `cli` (which masks everything, including the tick).
Today every critical section uses `cli`/`IrqGuard`, so a long IPC or
scheduler section delays even the highest-priority device IRQs, and there
is no way to say "mask PIC + background MSI-X, keep timer + SCHED IPI".

Constraint (verified, blocking): the live tick is the APIC timer at
**vector 64 / class 0x4**. Any raised class ≥ 0x40 would mask the tick ⇒
tick loss ⇒ RMS starvation. So either raised classes stay below 0x40
(guts the feature: only the PIC band becomes maskable) or the timer
moves above all raised classes. This spec chooses the move.

## 3. Design

### 3.1 Vector map change

`APIC_TIMER_VECTOR`: **64 → 0xE0** (class 0xE). One-constant change; all
kernel users are symbolic (`APIC::APIC_TIMER_VECTOR`) except three
hardcoded-64 sites that move with it (audited 2026-09-14):
`src/kernel/arch/x86_64/hal/apic.hpp:88` (the constant + its comment),
`src/kernel/arch/pci.cpp:537` (`g_vector_used[64]` bitmap entry — without
this the allocator could hand the tick vector to a device),
`src/kernel/test/test_cap_msix.cpp:212-220` (comment + `claim_slot(64)<0`
assertion → `0xE0`; vector 64 itself becomes an ordinary claimable MSI-X
vector). `IrqDelivery::claim_slot` (`irq_delivery.cpp:123`) and
`IrqDelivery::isr_entry` (`irq_delivery.cpp:123`-adjacent) use the symbol
and move automatically — no edit. Generated `docs/doxygen/html/*` is
rebuilt by the release flow, never edited.

Resulting fixed map (classes = vector bits 7:4; delivery iff
vector-class > TPR-class):

| Vector | Class | Role | Maskable? |
|---|---|---|---|
| 0–31 | — | CPU exceptions (TPR never gates faults/NMI) | never |
| 32 | 0x3 | PIT (legacy fallback path only) | only when LAPIC on |
| 33–47 | 0x3–0x4 | PIC via I/O APIC | yes (≥ PIC class) |
| 48–0xDF | 0x3–0xD | PCI/MSI-X allocations (`g_vector_used`) | yes (band-dependent) |
| 0x80 | 0x8 | Syscall trap (`INT n` — TPR never gates software interrupts) | never |
| 0xE0 | 0xE | APIC timer = system tick | **never** (above all raised classes) |
| 0xEC | 0xE | SCHED IPI (mailbox drain + reschedule arm) | never below SCHED class |
| 0xEF | 0xE | self-test vector | n/a |
| 0xFF | 0xF | Spurious | never |

`SCHED_VECTOR = 0xEC` is unchanged (class 0xE > SCHED class 0xD0 ⇒ an IPI
is always deliverable while a CPU sits at or below the SCHED class).

### 3.2 TPR classes

```cpp
TPR_CLASS_ACCEPT_ALL = 0x00,  // reset/idle state — everything deliverable
TPR_CLASS_PIC        = 0x30,  // mask legacy PIC band (≤ class 3)
TPR_CLASS_IPC        = 0x70,  // + mask background MSI-X below 0x80
TPR_CLASS_SCHED      = 0xD0,  // + mask all below 0xE0 (timer + IPI survive)
```

Sub-class bits (3:0) are forced to 0 on every write. No class ≥ 0xE0 may
ever be programmed (static rule; a `TPR_CLASS_MAX = 0xD0` bound + ENSURE
on the setter input range — reachable bad input returns an error code,
per CODING_STYLE §5; only the *impossible* (post-mask violation) ENSUREs).

### 3.3 Per-CPU shadow (`tpr_shadow`, `gs:0x40`)

- New `PerCpu` field `uint64_t tpr_shadow` at `gs:0x40` (`reserved` 475 →
  474; 483-u64 total + `static_assert` unchanged). **C++-only** (via
  `per_cpu_current()` / `cpu_index()`); no asm access, no linker alias.
- The shadow is the single source of truth for the intended class.
  Hardware TPR is re-asserted from the shadow at: `enable_local`,
  `init_ap`, ISR prologue (on mismatch), ISR tail, quiesce transitions.
- Own-CPU writes only, under IF=0. No cross-CPU TPR/shadow write exists
  (INV-TPR1/4). `MODE_NONE` (LAPIC off) ⇒ accessors are no-ops returning
  ACCEPT_ALL (PIT fallback path has no TPR hardware).

### 3.4 Accessor API (`APIC::`, `apic.hpp`/`apic.cpp`, x86_64-only)

- Hoist the `enable_local` `wr`/`rd` lambdas to file-static mode-gated
  helpers (identical mapping) so all paths share one tested route.
- `set_tpr_class(uint8_t cls)` — mask to 0xF0, return false (reject) for
  `> TPR_CLASS_MAX` (bool convention follows `send_ipi`).
- `get_tpr_class()` / `get_ppr()` — mode-gated read of `REG_TPR`/`REG_PPR`.
  PPR is diagnostic-only (read-only hardware: max(TPR, highest ISR)).
- `tpr_raise(cls)` — write only when `cls` exceeds current (monotonic up).
- `tpr_restore(cls)` — write only when lowering (monotonic down).
- All `noexcept`, no allocation, no loops (bounded single MMIO/MSR).
- `enable_local` / `init_ap` keep ACCEPT_ALL via direct `reg_wr` (not the
  setter: `enabled_` is committed by `init()` only after `enable_local`
  runs, and the setter is fail-closed while off — the original code wrote
  TPR=0 unconditionally here); SVR/LVT/ESR order unchanged.

### 3.5 `TprGuard` (new `src/kernel/arch/hal/tpr_guard.hpp`)

RAII: ctor saves own-CPU shadow, `tpr_raise(cls)`, updates shadow;
dtor `tpr_restore(saved)` + shadow write-back. Non-copyable,
non-movable, `[[nodiscard]]`. Lives in its OWN header (not inside
`irq_guard.hpp`): it needs the `APIC` + `PerCpu` declarations and
`irq_guard.hpp` is too widely included to take those dependencies
(include-cycle risk). **Does NOT replace `IrqGuard`**: every
`TprGuard` use site keeps its existing `scheduler_lock_` + IF=0
discipline (INV-TPR2); TPR only narrows *which* vectors may preempt.
Documented `cli`-vs-TPR rule: `cli` = mask everything (keep for
lock-holding sections); TPR = mask a band (for long IRQ-tolerant
sections that must still tick).

### 3.6 ISR prologue / tail (`handle_interrupt_c`, `kernel.cpp:1462`)

- Prologue (after vector decode, before `IrqDelivery` dispatch): when the
  local APIC is enabled (`APIC::is_enabled()`, x86_64 only — never touch
  LAPIC MMIO/MSRs when disabled), assert hardware TPR == calling-CPU
  shadow; re-assert from shadow on mismatch (stale-TPR self-heal).
  PPR sampled to entry diagnostics only.
- Tail: existing dispatch + **EOI first, TPR-restore second** on every
  consuming path (generic tail incl. PIC EOI for 32–47, SCHED IPI
  handler, `IrqThread` custom-ack path). Restore-before-EOI would
  re-deliver the same vector ⇒ double EOI (INV-TPR7).
- `IrqDelivery::isr_entry` keeps slot-lock serialize + single EOI; adds
  the class rule: PIC-kind 33–47 run at/below `TPR_CLASS_PIC`, MSI-X
  48+ at/below caller shadow; no vector remapping. Threaded handlers
  inherit the ISR shadow and must not raise except via `TprGuard`.
- Timer paths (`timer.cpp` BSP/AP branches) assert shadow ACCEPT_ALL on
  entry and restore before return; TSC-deadline/periodic re-arm stays
  outside any raised region.

### 3.7 SMP / quiesce / snapshot interaction (#25 C1 constraints hold)

- `ap_main`: after GS_BASE + IDT + `enable_local`, assert shadow and
  hardware TPR are ACCEPT_ALL before `sti` (AP FPU tripwire unchanged).
- `quiesce_enter`/`quiesce_exit`: re-assert ACCEPT_ALL on the calling
  CPU's shadow + hardware (own-CPU only); arm-cancel order unchanged.
- `send_ipi` ICR poll: no TPR change across the bounded poll; mailbox
  publish/drain/generation protocol unchanged.
- Test snapshot/restore extends the `test_isolate` POD with the 8 per-CPU
  shadows (bounded, `CONFIG_MAX_CPUS` u64s).

## 4. What TPR does NOT do (non-goals)

- Never replaces IF=0 + `scheduler_lock_` at task-context lock sites, nor
  `try_lock` + skip-and-retry at ISR sites.
- Exceptions, NMI, SMI, `INT n` (syscall 0x80) are not gateable by TPR
  (hardware rule) — the design does not rely on masking them.
- No remote-CPU TPR programming (would break core isolation).
- riscv64/aarch64: untouched (no LAPIC; deferred with their SMP, as #27).

## 5. Test strategy (stub-first, new class `apic_tpr`)

1. Shadow defaults ACCEPT_ALL on BSP and AP (`percpu_init_*`).
2. `set`/`raise`/`restore` round-trip via **both** x2APIC and xAPIC paths
   (mode injected as existing tests do); sub-class bits forced zero;
   `> MAX` rejected.
3. PPR read does not mutate TPR.
4. Prologue mismatch re-asserts from shadow (unit-level via accessor
   sequence; live ISR coverage via existing timer/IRQ classes).
5. `PerCpu` still exactly one page; `gs:0x40` offset frozen like 0x20.
6. Regressions: `per_cpu`, `sched_affinity`, `smp_sched` (smp2),
   `smp_bringup`, `irq_delivery`, `irq_threaded`, `hal_apic`,
   `hal_timer`, `timing_core`, `safe`/`selftest`, then debug `all` +
   release `all`.

## 6. Invariants (normative)

- **INV-TPR1:** per-LAPIC state; own-CPU writes only; `mode_` stays
  BSP-authoritative, mirrored by `init_ap` before any x2 touch.
- **INV-TPR2:** TPR never replaces IF; lock discipline unchanged.
- **INV-TPR3:** class semantics = vector bits 7:4; delivery iff vector
  class > TPR class; PPR = max(TPR, top ISR), diagnostic only.
- **INV-TPR4:** shadow is source of truth; HW re-asserted from it at
  enable/init/prologue/tail/quiesce; own-CPU + IF=0 only.
- **INV-TPR5:** timer 0xE0 and IPI 0xEC are class 0xE, above every
  raised class (≤ 0xD0) — the tick and cross-CPU wake are unmaskable.
- **INV-TPR6:** vectors 0–31, 0x80, 0xEC, 0xFF keep existing behavior;
  `IrqDelivery` claim window unchanged.
- **INV-TPR7:** EOI-before-restore on every consuming path.
- **INV-TPR8:** #25 C1 AP constraints (CPU0 user clamp, FPU tripwire,
  shared IDT before `sti`, start-gate/quiesce order) unchanged.
