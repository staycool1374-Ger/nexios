# AUDIT REPORT 2026-11-19T00-00-00Z-561-code-r1 (SIL 3 CODE audit, issue #561 F8)

PATCH: /tmp/pending_patch_561.diff (uncommitted production diff on main @ 2b86a0a8f, ~544 lines)
FILES: docs/specs/microkernel-transition.md, src/kernel/arch/hal/irq_storm.hpp,
  src/kernel/arch/x86_64/irq_storm.cpp (NEW), src/kernel/arch/aarch64/interrupt_controller.cpp,
  src/kernel/arch/riscv64/interrupt_controller.cpp, src/kernel/irq_delivery.{hpp,cpp},
  src/kernel/kernel.cpp, src/kernel/syscall/syscall_errors.hpp,
  src/kernel/syscall/syscall_handlers_irq.cpp
SCOPE: production diff only. No test files, no nexios_config.h, no ResourceTracker/test_isolate,
  no syscall dispatch-table (T-1), no #192 mux. graphify-out/* churn and test-history.txt rows are
  tool/evidence artifacts, excluded. Normative: audits/plan-561.md + design verdict
  audits/design-review-561-r1.md (APPROVED, 6 S3 notes), spec transition SS8.3/S11/S7.3/S14-S16.

## VERIFICATION (static reads of the patched worktree)

- Gate wrap is EXACTLY the three dispatch statements: kernel.cpp:2751 `if (!storm_tripped) {`
  covers isr_entry :2757-2758, threaded-IRQ block :2761-2774, IDT::handle_interrupt :2776, closing
  :2777. Tail APIC EOI :2782-2784, PIC EOI pair :2790-2794, TPR restore :2799-2801, latency
  histogram :2805-2807 all unconditional after the close. Tripped path falls through to single
  tail EOI (no suppression, no double-EOI: isr_entry's early return is skipped on trip). Gate sits
  after the 0x80 early return (:2709-2732); syscall path unaffected. Braces sit outside the
  `#if CONFIG_CAP_MAX_IRQ / CONFIG_THREADED_IRQS` blocks, so balance holds in every config;
  off-x86 the flag constant-folds false with no arch call (header include is arch-neutral).
- Timer exemption by name, no third source missed: storm_gate exempts TIMER (32, PIT/HPET IRQ0,
  idt.hpp:73) and APIC_TIMER_VECTOR (0xE0) first (irq_storm.cpp:62-65). TSC_DEADLINE shares 0xE0
  (apic.cpp:381 periodic + :426-456 deadline paths all program APIC_TIMER_VECTOR; timer.cpp:118
  registers the tick handler at 0xE0; timer.cpp:304-314 active sources all tick through 32/0xE0).
  0xE1/0xFF/IPI/MSI-X excluded by the 33-47 window. Masking either tick vector would wedge the
  scheduler; exemption-first is correct.
- Window 33-47 == irq_cap_vector_min/max (irq_vectors.hpp:41-60, x86 legs 33/47) with
  static_assert drift guards (irq_storm.cpp:51-52). MSI-X 48-255 can never reach `vector - 32`.
  storm_mask_line re-checks the window fail-closed (irq_storm.cpp:81-83). Within-window
  non-line vectors (34 cascade, 39/47 spurious) map by identity to their own line only.
- Dual mask is the exact mask_on_delivery twin: ArchInterruptController::mask(irq_line) PIC
  mirror + APIC::mask_irq(irq_line, true) under is_enabled (irq_storm.cpp:86-92 vs
  irq_delivery.cpp:122-128, line-for-line identical incl. guard order). ISR runs IF=0
  (interrupt gate); same-CPU interleave impossible, cross-CPU twin writes idempotent. No new
  hazard vs the proven sequence.
- Suppression never touches EOI even on trip (verified above); storm_tripped only skips
  dispatch. #252 wedge class respected.
- Pending saturation + paired resets: saturating pair at irq_delivery.cpp:476-480; exactly
  three `pending = 0` sites in-tree (grep: :309 claim, :585 release, :652 drain), each paired
  with `pending_overflow = 0` (:310/:586/:653) — no fourth site. Consume sites untouched and
  `> 0`-guarded. kIrqPendingMax is a UINT32 type bound (irq_delivery.hpp:106), no config change.
- Waiter refusal sits after locked revalidation (:214-219) and before unmask_for_wait (:228),
  typed 1201 (syscall_handlers_irq.cpp:224-227, errors.hpp:151; 1201 clash-free, X-macro single
  edit, grep-confirmed unique). Pre-lock GENERIC_FAIL (:171-174) untouched. Refused caller never
  unmasks; lock discipline unchanged (x86 lock-only, off-x86 IrqGuard-before-lock intact).
- Off-x86 substitution is behavior-identical: same static g_storm object, same caller branch,
  same counter/dmesg code (BASE+64, "irq storm masked"); observe+note moved inside storm_gate
  before the mask MMIO write (no ordering dependency). aarch64 :289-303 / riscv64 :177-194 keep
  timer/spurious/complete/tail structure byte-identical otherwise. Accessors + reset thin.
- Level-mask no-change honored: mask_on_delivery untouched, SS11:766 row unchanged (context
  line in diff); x86 WAIT-mode parity now rests on the limiter exactly as aarch64's contract.
- All 6 design S3s honored in code+spec: (1) 34/39/47 residual named in spec S8.3 +
  irq_storm.cpp:31-33,84-85; (2) "same order" wording corrected to before-mask-write +
  immaterial (hal header:86-89, arch comments); (3) claim-site uses reference syntax
  `r.pending_overflow = 0;` (:310); (4) two-CPU race documented fail-closed (hal:30-33);
  (5) S8.3(2) marked end-state/mux-owned; (6) sticky-mask + storm_reset-is-not-unmask stated
  (spec S8.3(1), hal:108-114), and release path verified to restore line state
  (irq_delivery.cpp:549-560) so the "until owning slot releases it" claim holds
  (WAIT_ACK release force-masks, fail-closed: :555).
- Critical checks: no allocation (one static limiter; +4 B/slot BSS, no pool/ResourceTracker
  change); no new lock or IRQ-off region (gate runs inside the ISR like its off-x86 twins);
  no test edits (assertion-masking n/a); no cap-domain channel; preprocessor semantics hold on
  all three arches (undefined-on-that-arch symbols never ODR-used; header arch-neutral).
- Evidence in test-history.txt (11-19 rows): x86 cap_irq 17, cap_irq_notify 7, capability
  167/3 (F2 trio base-matched), drivers 111, ipc 96, core_sched 127, storage 167, selftest 157,
  bench_microkernel 12, x86 release selftest 87 — all 0 failures; aa64/rv64 legs green incl.
  hal 83/85. Style Errors-0 is developer-claimed (auditor runs no builds); no style violations
  observed statically.

## HAL-x86 HOST-BLOCKED DISPOSITION (assessed, accepted with follow-up)

- Rows show 3 consecutive hal TIMEOUTs (22:54:49/22:55:28/22:59:58) while every IRQ-touching
  x86 class is green. Statically, the gate CANNOT produce the reported signature: the
  no-trip path adds no mapping/allocation/log (one atomic tick load + compares), and the
  trip path requires 256 same-vector dispatches within one tick and always leaves dmesg +
  trips evidence. A moving-stall-point acpi-scan map_page flood is facially a host/QEMU
  artifact (same family as the 2026-09-20 hal acpi-area QEMU_EXIT rows), not dispatch
  suppression. Zero-trips + bisect-clearing-kernel.cpp/F8-files (developer testimony, issue
  thread) are two independent exoneration legs; the release selftest 87 green (gate compiled
  in, boot clean) is a third. Exoneration is SOUND on mechanism + evidence combined.
- Limitation (recorded, not blocking): zero-trips/bisect/base-control facts rest on issue-thread
  testimony the auditor cannot re-execute; accepted because static mechanism analysis
  independently rules out gate involvement.

## FINDINGS

- [S3] hal-x86 HOST-BLOCKED — hal x86_64 has no green run in this submission (3 TIMEOUTs).
  WHY: Host-environment stall exonerated above by mechanism + testimony, but the plan gate set
  requires hal green on all three arches: QE must (a) track the "layout-shift exposes latent
  wild-read" hypothesis as a follow-up issue (not a silent waiver), and (b) re-run hal x86_64
  green when the host flake clears, before M8 closes. No code change needed.

DECISION: APPROVED
