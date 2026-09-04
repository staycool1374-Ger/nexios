# FPU/SIMD State in Context Switch — Fixed-Offset Save Area

**Doc ID:** NEX-SPEC-2026-08-23-003
**Status:** IMPLEMENTED (issue #93, v0.4.3 — 2026-09-03)
**Milestone target:** v0.4.3
**Inspiration:** Cyjon `kernel/task.asm` (FXSAVE64/FXRSTOR64 to fixed
KERNEL_STACK offsets on every switch).
**Related:** `docs/specs/drivers.md` §6 (vector 7 lazy-FPU path),
`src/kernel/kernel.cpp:1326ff`, `src/kernel/core/global_state.cpp:67`,
`docs/specs/test-harness.md` §116.

## 0. Implementation reconciliation (issue #93)

The TCB already carried `alignas(16) uint8_t fpu_state[512]` + `fpu_used`
(docs §1 assumed it absent). The landed deltas:

- **Alignment** 16 → **64** (`alignas(64) uint8_t fpu_state[512]`, task.hpp):
  FXSAVE requires 16; 64 avoids split-line stores (paper §3.3). Audited: no
  `offsetof`/layout consumer in the tree; fields are name-accessed.
- **`fpu_state_gen`** (task.hpp): `uint32_t`, bumped on every `fxsave` of a
  task's state (#NM handler + clone flush). 0 = never saved. Kept
  unconditional (not CONFIG_DEBUG) for debug/release symmetry.
- **INV-FPU2 reentrancy** (kernel.cpp vector-7): added an explicit `cli` as
  the first statement of the #NM branch. Verified hardware-guaranteed already:
  vector 7 is an interrupt gate (`0x8E`, idt.cpp:66) → IF cleared on entry;
  the kernel is compiled `-mgeneral-regs-only` so kernel code can never raise
  #NM inside an ISR; `isr_common` cli's after the C handler (isr_stubs.asm:156)
  and iret restores the interrupted RFLAGS. The `cli` makes it SMP-proof.
- **S1 stale-restore fix (NEW, found during implementation):** the original
  handler `fxrstor`'d whenever `prev_fpu_owner != current`. In the
  armed-switch window (CR0.TS published by a deferred arm, switch not yet
  applied), `prev_fpu_owner == current` — fxrstor then clobbered the owner's
  LIVE registers with stale/zero TCB state. Fix: when `prev == current`,
  leave the registers untouched (they already hold current's live state).
  Regression test `fpu_nm_own_arm_no_clobber`.
- **`fpu_nm_depth_max`** (global_state.cpp): highest `isr_nesting_depth` seen
  inside #NM; reset in test_isolate restore; `fpu_nm_nesting_impossible`
  asserts it stays ≤ baseline + 1.
- **CR0.TS-on-switch-away: verified NOT a gap.** CR0.TS is set at both
  deferred-switch publish sites (scheduler.cpp arm + reschedule); the lazy
  chain (TS set on switch-away → #NM on next user → save prev / restore next)
  is coherent.

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

Landed in `TaskControlBlock`:
```
    bool fpu_used;
    uint32_t fpu_state_gen;            // bumped on every fxsave (unconditional)
    alignas(64) uint8_t fpu_state[512]; // FXSAVE format, 64-byte aligned
```

512 bytes × max tasks is bounded (e.g. 64 user tasks ⇒ 32 KiB); fork deep-copy
copies the area by value like any other TCB field (task.cpp clone: memcpy 512
+ fpu_state_gen copy).

### 3.2 Reentrancy rule (landed)

**Binding rule: #NM handling runs with interrupts disabled until the FPU
owner swap completes.** Verified hardware-guaranteed: vector 7 is an interrupt
gate (IF cleared on entry, idt.cpp `0x8E`); the kernel is
`-mgeneral-regs-only` (kernel code can never raise #NM inside an ISR);
`isr_common` cli's after the C handler. An explicit `cli` was added as the
first statement of the #NM branch (kernel.cpp) to make the invariant
SMP-proof and pin it against a future trap-gate change. Documented in
drivers.md §6 as depth invariant: #NM never nests.

### 3.3 Alignment & correctness details

- FXSAVE requires 16-byte alignment; `alignas(64)` covers it and avoids
  split-line stores (landed).
- **S1 stale-restore fix (found during implementation):** #NM with
  `prev_fpu_owner == current` (armed-switch window) must NOT fxrstor stale/zero
  TCB state over live registers — the registers already hold current's live
  state. The handler now skips the restore in that case.
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

## 5. Test Plan (landed — class `fpu_invariants`, 4 TF_KERNEL)

New non-filtered file `test_fpu_inv.cpp` (the legacy test_fpu*.cpp files are
filtered out of the x86_64 build for GCC-16 / -mgeneral-regs-only; this class
uses only memory-operand x87 asm + arch::* helpers):
1. `fpu_nm_no_alloc` — PMM + ResourceTracker delta == 0 across forced #NM
   storms (INV-FPU1).
2. `fpu_nm_nesting_impossible` — `fpu_nm_depth_max <= baseline + 1` across a
   storm (INV-FPU2).
3. `fpu_save_area_alignment` — static_assert offset%64==0 + sizeof==512 +
   alignof==64; runtime check per created task.
4. `fpu_nm_own_arm_no_clobber` — regression for the S1 stale-restore fix.

Existing lazy-switch ABI compatibility: `atomic_context_switch` +
`task_core` (clone memcpy) run as regressions; the old test_fpu*.cpp stay
filtered (their ABI role is absorbed by the new tests).

Validation: selftest, debug/release all, test-history rows.

## 6. Non-Goals

- XSAVE/XSAVEC compact layouts, AVX-512 state — revisit when a workload
  needs it; FXSAVE keeps the area size frozen at 512 B today.
