# AUDIT REPORT 2026-11-19T00-00-00Z-561-r1 (SIL 3 DESIGN review, issue #561 F8)
PATCH: n/a — design review of audits/plan-561.md (no pending_patch.diff; no code committed)
FILES: audits/plan-561.md; src/kernel/arch/hal/irq_storm.hpp, src/kernel/arch/x86_64/irq_storm.cpp (new), src/kernel/kernel.cpp, src/kernel/arch/aarch64/interrupt_controller.cpp, src/kernel/arch/riscv64/interrupt_controller.cpp, src/kernel/irq_delivery.hpp, src/kernel/irq_delivery.cpp, src/kernel/syscall/syscall_handlers_irq.cpp, src/kernel/syscall/syscall_errors.hpp, docs/specs/microkernel-transition.md

BASE: main @ 2b86e0a8f (#558 F5) — verified via git log; plan-561.md untracked. Branch main (production).
SCOPE: design audit per PROMPT-audit evidentiary standard (static reads only; no test execution). All file:line anchors below re-read in-tree unless marked (plan).

## Normative basis
- microkernel-transition.md §8.3(1):538-550 (storm limiter off-x86 only; pending unsaturated; recipient-gate hardening), §8.3(2):551-555 (level-mask / saturate / typed-refusal invariants), §11:765-766 (parity matrix), §14 M8:866 (gate classes capability+drivers), §15 F8:891, §16 Q8:917 (identical limiter port, named timer exemption, per-vector knob REJECTED, A/B blind spot follow-up-only).
- irq-preemption-contract.md §2 (unranked locks incl. slot lock; M8 re-audit).
- report-2026-11-14T18-37-32Z-512-547.md:49-52 (single-waiter = hardening, not defect fix).
- Prior IrqDelivery audit constraints (arm-under-lock, slot-reuse revalidation, waker-owns-wakeup) — plan touches none of those paths (verified: no arm/isr-wake edit in steps 7-12).

## S1 verification (all PASS)
- Gate placement: wrap covers exactly the three dispatch statements (kernel.cpp:2738 isr_entry, :2745-2752 threaded, :2755 IDT) with tail EOI (:2760-2772) + TPR restore (:2777-2779) + histogram (:2783-2786) unconditional. Tripped path falls through to tail EOI (APIC+PIC for 32-47), so no EOI-suppression path and no #252-class wedge. Gate sits after the 0x80 early return (:2708-2731), syscall path unaffected.
- Timer exemption by name covers BOTH x86 tick sources and no third exists: InterruptVector::TIMER=32 (arch/hal/idt.hpp:73, PIT/HPET IRQ0) and APIC_TIMER_VECTOR=0xE0 (arch/x86_64/hal/apic.hpp:151); TSC_DEADLINE reuses 0xE0 (apic.cpp:381,396,426-456,510). 0xE1 sched-apply / 0x80 / 0xFF excluded by window. Masking either tick vector would wedge the scheduler — exemption is first in storm_gate. Timer::ticks() is a lock-free __atomic_load_n (timer.cpp:185-186); APIC::mask_irq/ioapic_masked exist (apic.hpp:130,146; apic.cpp:157,182).
- Window 33-47 == irq_cap_vector_min/max (irq_vectors.hpp:42-54) with static_assert drift guard; MSI-X/IPI (48-255), 0xE0/0xE1/0xFF excluded, so vector-32 can never mask a foreign line. Within-window non-line vectors (34 cascade, spurious 39/47) map by identity to their own line only — see S3-1.
- Dual mask is the exact twin of mask_on_delivery's x86 branch (irq_delivery.cpp:119-128): ArchInterruptController::mask = PIC mirror (hal/interrupt_controller.hpp:69-74) + APIC::mask_irq guarded by is_enabled. ISR runs IF=0 (interrupt gate); same-CPU interleave impossible, cross-CPU interleave benign (idempotent mask writes) — no new hazard vs proven sequence.
- Saturation pairing: exactly three `pending = 0` sites confirmed by grep (irq_delivery.cpp:309 claim_slot, :578 release_slot_idx, :644 drain_task), no fourth reset site; step 9 pairs overflow reset at all three. Decrease sites (:221,311,316) already >0-guarded, untouched correctly.
- Waiter refusal sits after locked revalidation (:214-219) and before unmask_for_wait (:220); refused caller never unmasks; QE asserts line stays masked. No new lock, no lock-order interaction (gate takes no lock, touches neither scheduler_lock_ nor slot lock); no new IRQ-off region — #545/#546 surface untouched.
- INV-9 correctly framed as hardening (recipient gate :171 already refuses distinct tasks), never as a defect fix — spec-faithful per §8.3(1) and audit #547.

## S2 verification (all PASS)
- Level-mask no-change is spec-faithful parity, not scope-dodging: §11:766 documents aarch64 WAIT as limiter-backed without mask-on-delivery; the plan makes x86 WAIT exactly that contract. §8.3(2) mask-on-delivery is end-state (mux era) and M8 explicitly sequences hardening before the #192 irq-mux config role, which owns mask semantics. Decision recorded with three technical reasons + spec pointer — see S3-5 for the residual.
- New error IRQ_ALREADY_WAITING 1201: VSPACE block ends at 1112 (syscall_errors.hpp:149), so 1201 is clash-free; X-macro single edit generates error_string; face matches SYNC_ALREADY_WAITING 409 precedent; no userspace mirror exists (no SYS_ERR_ under src/libc or userspace PDs) — purely additive internal code. Pre-lock GENERIC_FAIL (:171) deliberately kept (ABI-visible), recorded for #192 — honest scoping.
- No-new-syscall vs T-1 closed set: no dispatch-table change of any kind. Cross-arch substitution is same-object/same-counter with per-function objdump evidence specified — adequate parity evidence for design stage.
- Gate set = M8 row (capability, drivers) on all three arches plus hal/ipc/core_sched/storage/selftest/bench; 2x3 matrix deferred to M8/#192 is a legitimate deferral (F8 is hardening, neither a flip nor a kernel deletion per §14 gate rule).

## S3 verification (all PASS)
- check-7 retrieval present with per-query dispositions and ignored-item notes (plan lines 7-11).
- nexios_config.h untouched by design: CONFIG_IRQ_STORM_LIMIT 256 already exists (Q7 froze it unchanged); kIrqPendingMax is a type bound, not a tunable.
- Test sanctity held (QE-owned on testbed, stubs kept, mapping to #192 retirement recorded); count arithmetic verified against test_expected_counts.hpp rows: hal 130/83/85→131/84/86 (+1×3), cap_irq 17/15/15→19/17/17 (+2×3: pending + waiter tests), capability 161/0/0→163/0/0 (+2 x86-only aggregate), bench_microkernel unchanged at 12. Stuck-line test uses seams (snapshot/restore + local unit run) with bounded ≤512-iteration loops — TCG-safe by construction.
- No #192 mux built (config-role handoff only); MSI-X flood and A/B-alternation blind spot documented as follow-up-only per Q8; r->lock_ stays unranked per contract §2 with no new lock taken.

## FINDINGS
- [S3] plan-561.md step 5/81 (window claim) — vectors 34 (PIC cascade), 39/47 (spurious IRQ7/15) are non-line vectors INSIDE 33-47, so the window does not exclude literally all non-line vectors.
  WHY: Harmless by construction — the subtraction is an identity map (vector-32 → own line), so no foreign line can ever be masked; triggering needs 256 same-tick dispatches of that vector (i.e. a genuine stuck line), and the consequence direction is mask+log (fail-closed). Recommend naming the residual in §8.3 rather than claiming full exclusion.
- [S3] plan-561.md steps 2-4 (INV-12 "same order") — the substitution moves note_masked() BEFORE the controller mask write (original: observe→mask→note→dmesg; new: observe→note→mask→dmesg).
  WHY: Immaterial (trip counter vs MMIO write have no ordering dependency; per-function objdump will show only the inlined gate), but the "same order" claim is imprecise — correct the wording, keep the design.
- [S3] plan-561.md step 9 — reset pairing written uniformly as `r->pending_overflow = 0;`, but claim_slot (:309) uses reference syntax `r.pending = 0;`.
  WHY: Cosmetic plan-vs-tree mismatch only; the developer must adapt the edit to `r.pending_overflow = 0;` at :309. No semantic issue.
- [S3] plan-561.md INV-6 ("no lock") — the single-image g_storm uses plain uint64_t state, so concurrent same-line storms on two CPUs race (torn burst_/last_intid_).
  WHY: Pre-existing pattern shared verbatim with aarch64/riscv64 (Q8 mandates the identical limiter); failure direction is fail-closed (spurious trip masks+logs, missed trip delays masking by ≤1 tick). Document the race or consider atomics at the #192 pass; not a blocker for the identical port.
- [S3] plan-561.md step 15 — §8.3(2) "level lines SHALL be masked on delivery" remains literally open for WAIT mode until #192.
  WHY: Accepted as sequenced scope (parity now, mux semantics later), but the §8.3 sync edit should mark (2) explicitly as end-state/mux-owned so a future reader does not cite the F8 commit as satisfying it.
- [S3] plan-561.md step 6/INV-3 — a storm-masked line with no owning slot (kernel-handler line, or any line under a CAP_MAX_IRQ=0 build) has no unmask path short of slot release/reboot; storm_reset() restores burst state only.
  WHY: Fail-closed direction and reachable only after a genuine 256/tick storm (broken/stuck hardware), where masking beats the status-quo livelock; test teardown restores via snapshot. State the consequence explicitly in §8.3 so the stickiness is a documented property, not a surprise.

DECISION: APPROVED
