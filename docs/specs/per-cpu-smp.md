# Per-CPU Foundation & SMP Bringup Skeleton

**Doc ID:** NEX-SPEC-2026-08-23-002
**Status:** DRAFT
**Milestone target:** v0.4.4 (design now, land with Phase 5 SMP)
**Inspiration:** Cyjon `kernel/task.asm` (`KERNEL.task_cpu_address[lapic_id]`)
and `kernel/init/ap.asm` (uniform AP init path); noted caveat: Cyjon's own
source marks its AP task registration as racy — we adopt the *structure*, not
the synchronization.
**Related:** `docs/specs/scheduler.md` §8 ("isr_nesting_depth → per-CPU asm —
deferred to SMP"), `docs/specs/memory.md`, `docs/specs/drivers.md` §6.

## 1. Current State (verified)

NexIOS is single-core with two gs-relative slots already in place:

```asm
; syscall_entry.asm
mov [gs:0x00], rsp        ; user RSP saved
mov rsp, [gs:0x08]        ; kernel stack loaded
```

Global state that must become per-CPU under SMP (audit findings):
`isr_nesting_depth`, `irq_entry_tsc` (both plain `[rel …]` in
isr_stubs.asm:91ff), `fpu_owner` (global_state.cpp:67), the scheduler
run-queue lock, and `hhdm_modified_` (VAR-17, flagged "re-audit under SMP").

## 2. Cyjon's Model and What We Take From It

Cyjon reads the LAPIC ID, indexes `KERNEL.task_cpu_address[]`, and gets the
current-CPU context in three instructions — no segment-register gymnastics.
The idea generalizes: **one flat array indexed by LAPIC ID, filled during BSP
+ AP bringup, read-only afterwards.**

What we deliberately do differently:

| Aspect | Cyjon | NexIOS |
|---|---|---|
| Index source | rdmsr-free MMIO read each time | GS_BASE MSR set once per CPU at init; struct cached in gs |
| AP init sync | spinlock byte + known race ("-_-" comment) | INIT-SIPI with documented rendezvous + timeout panic |
| Task registry | global array, unlocked | existing Scheduler TCB lists behind scheduler_lock_ |

Reason to prefer GS_BASE over repeated LAPIC-ID reads: our syscall entry
*already* depends on gs (user-RSP/kernel-stack swap), so the per-CPU block
costs nothing extra; a MMIO LAPIC read per ISR would add ~100+ cycles to
every interrupt — unacceptable for RT latency budgets.

## 3. Design

### 3.1 Per-CPU block layout

```
struct PerCpu {                 // 4 KiB-aligned, one page per CPU
    uint64_t user_rsp;          // gs:0x00  (existing — ABI frozen)
    uint64_t kernel_rsp;        // gs:0x08  (existing — ABI frozen)
    uint64_t cpu_id;            // gs:0x10  logical id == index
    uint64_t lapic_id;          // gs:0x18
    uint64_t isr_nesting_depth; // gs:0x20  moved from .bss
    uint64_t irq_entry_tsc;     // gs:0x28
    void *   fpu_owner;         // gs:0x30  (TaskControlBlock*)
    void *   current_task;      // gs:0x38
    ... reserved to page end
};
PerCpu per_cpu[CONFIG_MAX_CPUS];   // CONFIG_MAX_CPUS default 4
```

Migration is mechanical: every `qword [rel isr_nesting_depth]` becomes
`qword [gs:0x20]`; C accesses go through `arch::per_cpu()->field`. A
single-core build keeps `CONFIG_MAX_CPUS == 1` so semantics are provably
unchanged (the gs base points at `per_cpu[0]`, set during boot before the
first `cli` region ends).

### 3.2 AP bringup skeleton (Phase 5)

1. BSP parses MADT, allocates per-Cpu pages, fills `lapic_id` per entry.
2. For each AP: send INIT, SIPI (vector of `ap_trampoline`), SIPI again after
   200 µs if the CPU's `started` flag (in its PerCpu page, written by the AP
   itself) is still clear. Second SIPI failure ⇒ controlled panic naming the
   LAPIC id — fail-closed, never half-alive cores (Cyjon's lesson inverted).
3. Trampoline (identity-mapped, < 4 KiB): load the page's own GDTR, set
   GS_BASE = this PerCpu page (wr{fs,gs}base MSRs if available, else WRMSR
   path), load CR3 (shared kernel PML4 — see memory.md kernel-half rules),
   far-jump to `ap_main` (high half), unmap trampoline after all APs are up.
4. `ap_main`: GDT/TSS per CPU (TSS registered in its own GDT slot), IDT is
   shared/read-only, enable LAPIC, mark started, `sti`, enter idle loop that
   pulls work via the (Phase 5) scheduler hook.

### 3.3 Sequencing rule

No AP enters the scheduler until the ready-queue generation counter scheme
(scheduler.md §7 snapshot machinery) is extended with a per-CPU run queue;
until that lands, APs park in idle and only service IPI-directed IRQ work.
This gives us bringup testing *before* scheduling correctness work.

## 4. Invariants

- INV-PC1: gs:0x00/0x08 semantics unchanged — existing syscall_entry/isr
  epilogues need no edits beyond nesting-depth relocation.
- INV-PC2: `isr_nesting_depth` is always accessed relative to gs; a
  grep-level CI check forbids `rel isr_nesting_depth`.
- INV-PC3: an AP that fails to start within its deadline causes panic, not
  silent degradation.

## 5. Test Plan

Single-core first (no QEMU -smp change): class `per_cpu` verifies gs-slot
round-trip and nesting-depth relocation against existing IRQ tests (must pass
unmodified — proves ABI stability). SMP stage (QEMU `-smp 2`):
`ap_bringup_two_cpus` (both report started), `ipi_ping_pong`,
`ap_failure_panic` (block one AP's SIPI vector, expect panic string).
Deadline-monitor histograms recorded per-CPU.

## 6. Open Questions

- x2APIC vs xAPIC mode (MADT flags decide; x2APIC removes the MMIO read
  entirely, strengthening §2's argument for GS_BASE indexing).
- Whether `current_task` migration out of Scheduler globals happens here or
  with the Phase 5 scheduler split — proposed: here, mechanically.
