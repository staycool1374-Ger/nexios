# FPU/SIMD State in Context Switch — Fixed-Offset Save Area

**Doc ID:** NEX-SPEC-2026-08-23-003
**Status:** DRAFT
**Milestone target:** v0.4.3
**Inspiration:** Cyjon `kernel/task.asm` (FXSAVE64/FXRSTOR64 to fixed
KERNEL_STACK offsets on every switch).
**Related:** `docs/specs/drivers.md` §6 (vector 7 lazy-FPU path),
`src/kernel/kernel.cpp:1326ff`, `src/kernel/core/global_state.cpp:67`,
`docs/specs/test-harness.md` §116.

## 1. Current State (verified)

NexIOS already implements **lazy FPU** — the better scheme, and one Cyjon does
not have:

- `fpu_owner` is a single global `TaskControlBlock*` (global_state.cpp:67).
- On `#NM` (vector 7) kernel.cpp:1326ff saves the previous owner's state and
  restores the incoming task's; CR0.TS is manipulated so an unused FPU costs
  zero on switch.
- isr epilogue / scheduler switch path carries no FPU cost for tasks that
  never touch XMM.

So the *mechanism* exists. The gaps are RT-relevant details:

1. `fpu_owner` is a plain global with acquire loads (`__atomic_load_n`) but
   the save/restore sequence itself is not reentrancy-audited against nested
   interrupts (drivers.md caps nesting at depth 2 — can a #NM nest inside a
   timer ISR that then switches tasks?).
2. Save buffer location/provenance: where does FXSAVE write? If it is a
   per-task heap allocation, we violate "no dynamic allocation in the IRQ
   path" (drivers.md §6 principle 1) at #NM time.
3. No XSAVE/opt-in for AVX states; FXSAVE's 512-byte layout is fine but must
   be 16-byte aligned and documented per TCB.

## 2. What Cyjon Contributes

Cyjon does eager save/restore to *fixed offsets inside the task's own kernel
stack region* — no pointer chase, no allocation, constant latency, trivially
SMP-safe later (state lives with the task, not in a global table). We adopt
the **placement discipline** (fixed, pre-allocated, per-task) while keeping
our lazy trigger.

## 3. Design

### 3.1 Fixed save area per user TCB

```
TaskControlBlock gains:
    alignas(64) uint8_t fpu_save[512];   // FXSAVE format
    uint32_t fpu_state_gen;              // bumped on each save (debug)
    // set at create_user()/fork time — NEVER allocated at IRQ time
```

512 bytes × max tasks is bounded (e.g. 64 user tasks ⇒ 32 KiB) — acceptable;
fork deep-copy copies the area by value like any other TCB field.

### 3.2 Reentrancy rule (fixes gap 1)

Binding rule: **#NM handling runs with interrupts disabled until the FPU
owner swap completes** (`cli` around save/restore, `sti` only via the normal
epilogue). Rationale: the swap is ~60–100 cycles (two 512-byte stores), far
below any deadline quantum, and it removes the entire nested-#NM class.
Documented in drivers.md §6 as depth invariant: #NM never nests.

### 3.3 Alignment & correctness details

- FXSAVE requires 16-byte alignment; `alignas(64)` covers it and avoids
  split-line stores.
- After restore, execute `ldmxcsr` sanity: reserved bits set ⇒ #GP; validate
  once at task creation instead (init control word = 0x037F, MXCSR =
  0x1F80), tasks cannot change them except via explicit syscall later.
- Kernel tasks: TS set on entry to kernel-only work is unnecessary; keep
  current behavior (kernel avoids XMM or saves inline) — unchanged.

## 4. Invariants

- INV-FPU1: no dynamic allocation anywhere in the vector-7 path.
- INV-FPU2: #NM handler is non-interruptible between owner-swap start/end.
- INV-FPU3: `fpu_owner` transitions only under the swap critical section;
  after SMP (specs/per-cpu-smp.md) it moves into PerCpu (gs-relative).

## 5. Test Plan

Extend existing fpu coverage:
1. `nm_no_alloc` — PMM/MemPool delta == 0 across forced #NM storms.
2. `nm_nesting_impossible` — instrument: assert nesting depth stays ≤ its
   pre-#NM value during swap (debug counter).
3. `save_area_alignment` — static_assert + runtime check per created task.
4. Existing lazy-switch tests (test-harness dummy_save_rsp interplay) pass
   unmodified — proves ABI compatibility.

Validation: selftest, debug/release all, test-history rows.

## 6. Non-Goals

- XSAVE/XSAVEC compact layouts, AVX-512 state — revisit when a workload
  needs it; FXSAVE keeps the area size frozen at 512 B today.
