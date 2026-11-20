# PLAN: F8 (#561) — x86_64 IRQ parity: storm limiter, level-mask parity, `pending` saturation, waiter refusal

## RETRIEVAL (check-7 evidence — MCP graph + vault)

| # | Query | Disposition |
|---|---|---|
| 1 | `graphify query_graph "IRQ delivery storm limiter pending saturation per-arch masking"` | **used** — surfaced `IrqStormLimiter` (`arch/hal/irq_storm.hpp:35`, members `burst_`/`last_intid_`/`last_tick_`/`trips_`), `IrqRegistration::pending` (`irq_delivery.hpp:105`), `IrqGuard`, `PerCpu`. Confirms: single-slot limiter, no lock, `pending` is a bare `uint32_t` at the same table. |
| 2 | `graphify query_graph "irq_delivery pending bits storm detection aarch64 riscv64 x86 parity"` (181 nodes) | **used** — `IrqStormLimiter` reachable only from `aarch64/interrupt_controller.cpp` + `riscv64/interrupt_controller.cpp`; x86 arch sources contain **no** limiter node/edge. **ignored** — GPIO/urc/crash-dump/PMC nodes (no IRQ-delivery surface). |
| 3 | `graphify shortest_path IrqStormLimiter → irq_delivery.cpp` (3 hops via `aarch64/interrupt_controller.cpp`) and `→ x86_64` (5 hops only through the test-count table) | **used** — the limiter has no x86 import path: x86 dispatch lives in `kernel.cpp:handle_interrupt_c` (`IrqDelivery::isr_entry` at `:2738`), not in an arch controller. This is *why* the port cannot be a copy of the off-x86 file. |
| 4 | `graphify get_neighbors IrqStormLimiter` | **used** — 8 members, all plain integers + 4 methods; no shared state, no lock. Gate/mask/trip counter is the whole public surface a test needs. |
| 5 | `obsidian search_vault` ×6 (`storm`, `irq_delivery`, `M8`, `microkernel transition`, `IRQ delivery hardening`, `single waiter irq slot drain`) | **used** — `specs/microkernel-transition.md` §7.3:468, §8.3:538-565, §11:765-766, §14 M8:866, §15 F8:891, §16 Q8:917; `specs/irq-preemption-contract.md` §2 ("unranked locks … M8 re-audits"); `audits/report-2026-11-14T18-37-32Z-512-547.md:49-52` (§11 mask claim + "single-waiter is hardening, **not** a defect fix"); `audits/report-2026-08-29T23-50-42Z.md` (prior IrqDelivery audit: arm-under-lock, slot-reuse revalidation, waker-owns-wakeup — must not be disturbed). **ignored** — `audits/design-review-582-r5.md`, `audits/plan-582-final.md` ("storm" = scheduler park rearm counter, unrelated subsystem). |

Verified branch/issue state: `main` (production), issue #561 `OPEN`, milestone v0.6.1, labels `kernel:arch`/`sil3-relevant`, work-begun comment already posted. F1–F5 (`cap/vspace_map.*`, `cap/tcb_cap.*`, `ipc/` reply token, `cap/cap.cpp`) merged and **verified not to touch the IRQ surface**: the only `IrqDelivery::` callers in-tree are `irq_delivery.cpp`, `kernel.cpp:2738`, `cap/irq.cpp`, `cap/msix.cpp`, IPC-side task cleanup, and `src/kernel/test/*`.

Verified anchors used by this plan (file:line), all read in this session:

- `arch/hal/irq_storm.hpp:35-72` (`IrqStormLimiter::observe` :39, `masked_count` :55, `note_masked` :58, `reset` :61)
- `arch/aarch64/interrupt_controller.cpp:35` `g_storm`, `:242-284` `handle_gic_irq` (timer INTID 30 branch :253, storm branch :260-275)
- `arch/riscv64/interrupt_controller.cpp:30` `g_storm`, `:120-170` `handle_plic_trap` (timer code 5 :132, storm branch :148-165)
- `kernel.cpp:2424` `handle_interrupt_c`; `:2708-2731` `int 0x80`; `:2733-2740` `IrqDelivery::isr_entry`; `:2742-2753` threaded IRQs; `:2755` `IDT::handle_interrupt`; `:2757-2787` tail EOI/TPR/histogram
- `irq_delivery.hpp:105` `pending`; `:49-59` modes; `:70-74` kinds; `:108` `lock_`
- `irq_delivery.cpp:59-64` claim window; `:112-148` `mask_on_delivery` (x86 :119-128, PLIC no-mask :138-146); `:153-176` `unmask_for_wait`; `:288-316` `claim_slot` (`pending = 0` :309); `:445-516` `isr_entry` (`++r->pending` :473); `:542-553`+ `:578` (`pending = 0`); `:617-646` drain (`pending = 0` :644)
- `syscall/syscall_handlers_irq.cpp:150-329` `sys_irq_wait` (recipient fast-refusal :171, locked revalidation :214-219, `unmask_for_wait` :220, `pending` consume :221/:311/:316, `r->waiter = t` :237)
- `syscall/syscall_errors.hpp` (ranges 100…1112; highest `VSPACE_ACTIVE_SPACE = 1112`)
- `arch/hal/idt.hpp:73` `TIMER = 32`; `arch/x86_64/hal/apic.hpp:151` `APIC_TIMER_VECTOR = 0xE0`; `apic.cpp:157` `mask_irq`, `:182` `ioapic_masked`
- `arch/hal/irq_vectors.hpp:40-66` `irq_cap_vector_min/max/valid` (x86 33/47)
- `nexios_config.h:122-123` `CONFIG_IRQ_STORM_LIMIT 256`; `:604-605` `CONFIG_CAP_MAX_IRQ 16`
- build: `src/meson.build:122-134` arch TU discovery = `arch/*.cpp` (maxdepth 1) + `arch/<arch>/**.cpp`; mirrored by `tools/meson/tests/test_source_set.py:27-52`
- tests: `test/test_microkernel_transition.cpp:244` `irq_storm_x86_bounded` stub, `:257` `irq_pending_capped` stub (class `bench_microkernel`/`bench`); `test_expected_counts.hpp:99` cap_irq 17/15/15, `:318` hal 130/83/85, `:306` capability 161/0/0; `tools/meson/list_tests.py:24-43` arch class lists (`hal`, `capability`, `cap_irq`/`cap_irq_notify` off-x86)

## AFFECTED FILES

- `src/kernel/arch/hal/irq_storm.hpp` — declare the arch-wide gate contract (`storm_gate`, `storm_mask_line`, `storm_trips`, `storm_reset`) next to `IrqStormLimiter`; document the identical rule for all three arches and the x86 timer exemption.
- `src/kernel/arch/x86_64/irq_storm.cpp` — **new TU** (auto-discovered by `src/meson.build:125-134`): the single x86 `g_storm`, the 256/tick same-vector gate with the named timer exemption, the effective x86 mask (PIC mirror line + live I/O APIC entry), trip counter, test reset.
- `src/kernel/kernel.cpp` — `handle_interrupt_c`: call the gate after the `int 0x80` early return and wrap only the dispatch block (`IrqDelivery::isr_entry` / threaded IRQ / `IDT::handle_interrupt`) in `if (!storm_tripped)`; tail EOI/TPR/histogram stay unconditional.
- `src/kernel/arch/aarch64/interrupt_controller.cpp` — replace the inline `g_storm.observe()`/`note_masked()` sequence in `handle_gic_irq` with `arch::storm_gate()`/`arch::storm_mask_line()`; add the three accessors. **Behavior-identical** (same object, same order, same counter).
- `src/kernel/arch/riscv64/interrupt_controller.cpp` — same substitution in `handle_plic_trap` + accessors. **Behavior-identical**.
- `src/kernel/irq_delivery.hpp` — add `kIrqPendingMax`, `IrqRegistration::pending_overflow` (saturating), and the single-waiter invariant sentence to the class doc.
- `src/kernel/irq_delivery.cpp` — saturating `pending` increment with overflow accounting (:473); reset `pending`+`pending_overflow` **as a pair** at all three reset sites (:309, :578, :644).
- `src/kernel/syscall/syscall_handlers_irq.cpp` — `sys_irq_wait`: typed refusal for a second distinct waiter under the slot lock, **before** `unmask_for_wait`.
- `src/kernel/syscall/syscall_errors.hpp` — new `IRQ_*` error range (1201) so the refusal is typed (CODING_STYLE §5.3), no `GENERIC_FAIL` fallback for a reachable condition.
- `docs/specs/microkernel-transition.md` — §7.3:468 (`CONFIG_IRQ_STORM_LIMIT` users = all three arches), §8.3(1):538-550 (limiter exists on all three arches; x86 exemption named), §11:765 (`IRQ storm limiter: present` on x86_64).
- **Not touched:** `nexios_config.h` (no new bound justified — `CONFIG_IRQ_STORM_LIMIT 256` already exists and `kIrqPendingMax` is a type bound, not a tunable), `cap/` bounds (Q3/F9 turf), T-1 syscall table (no new syscall), `src/kernel/test/**` (test sanctity — QE owns tests on `testbed`), `task/*`, `ipc/*`, `scheduler*` (#545/#546 surface: no lock, no IRQ-off window added).

## INVARIANTS & CONCURRENCY BOUNDARIES

- **INV-1 (storm, all arches):** one limiter instance per kernel image; a *line-kind* vector dispatched `> CONFIG_IRQ_STORM_LIMIT` (256) times within one `arch::Timer::ticks()` value is masked, counted once (`trips_`), logged once (dmesg `BASE + 64`), and **not dispatched**; the tick vectors are exempt **by name**.
- **INV-2 (x86 exemption, load-bearing):** the gate must never count `InterruptVector::TIMER` (32, PIT/HPET tick) or `APIC::APIC_TIMER_VECTOR` (0xE0, xAPIC tick). Masking either stops the scheduler tick and wedges the system (``#26`` TPR/EOI class).
- **INV-3 (x86 mask completeness):** on an APIC-enabled kernel a PIC mask alone does **not** stop delivery — the I/O APIC entry is the live path (`irq_delivery.cpp:124-128`). The storm mask must therefore be the exact twin of `mask_on_delivery`'s x86 branch: `ArchInterruptController::mask(line)` **and** `APIC::mask_irq(line, true)` when `APIC::is_enabled()`.
- **INV-4 (no vector arithmetic on non-line vectors):** the x86 gate window is 33–47 only; `vector - 32` is reachable only inside that window. A MSI-X vector (48–255) must never reach the subtraction (it would mask a foreign PIC line — `irq_delivery.hpp:61-69` kind-gating rule).
- **INV-5 (EOI completeness):** suppressing dispatch must not suppress EOI/TPR restore. The tail (`kernel.cpp:2757-2787`) runs on every path; the wedge mode "ISR stuck, no further interrupts" (`:2527-2532`, #252) is exactly what this guards.
- **INV-6 (ISR lock discipline):** the gate adds **no lock** and takes none; it runs in the arch ISR with interrupts masked (x86 interrupt gate IF=0; aarch64/riscv DAIF/SIE set on entry). `Timer::ticks()` is a lock-free atomic load; `APIC::mask_irq`/`ioapic_wr` and `ArchInterruptController::mask` are lock-free MMIO. No new IRQ-off region, no new rank entry, no `IrqGuard` (already inside the ISR).
- **INV-7 (pending saturation):** `pending` is monotone non-decreasing except at the three documented consume/decrease sites (all guarded by `> 0`) and at the three reset sites; it never wraps. Overflow increments are counted in `pending_overflow` (also saturating) so no delivery is lost silently.
- **INV-8 (pending/overflow reset pairing):** `pending` and `pending_overflow` are reset together at `claim_slot`, `release_slot_idx`, `drain_task` — a slot is never handed out with a stale overflow count.
- **INV-9 (single waiter):** a slot has at most one waiter. A second distinct waiter is refused with a **typed** error under the slot lock, and a refused waiter must not unmask the line (`unmask_for_wait` runs only after the refusal check). This is a **hardening assertion, not a defect fix** (spec §8.3(1):537-549; audit #547 finding: `r->recipient != t` already refuses every distinct second task) — the plan must not present it as a bug fix.
- **INV-10 (test isolation):** the limiter is process-global state; any test that trips it must `storm_reset()` and restore the controller mask snapshot (PIC `snapshot()/restore()` + I/O APIC entry) or it leaks a masked line into the next test (ResourceTracker cannot see it).
- **INV-11 (ResourceTracker):** no new kernel resource type, no new pool, no allocation — `g_irq_regs[CONFIG_CAP_MAX_IRQ]` static; `sizeof(IrqRegistration)` grows 4 B (16 slots = +64 B BSS). No `test_isolate.cpp` layout change.
- **INV-12 (cross-arch neutrality):** off-x86 the ISR restructure is the *same object, same call order, same counter*; the shared-file change in `kernel.cpp` folds to `storm_tripped == false` on aarch64/riscv64 (constant false, no arch call).

## STEP-BY-STEP CHANGES

**Phase P1 — shared gate contract (header only, no behavior change)**

1. `arch/hal/irq_storm.hpp`: keep `IrqStormLimiter` unchanged. Extend the file comment: the rule is identical on all three arches (256/tick, same INTID, per-image single instance), and the tick vector is exempt *structurally* off-x86 and *by name* on x86. Add declarations after the class:
   - `bool storm_gate(uint64_t intid, uint64_t now_tick);` — "true ⇒ this dispatch hit the limit; the caller **must** call `storm_mask_line()` and emit one dmesg ERROR (`BASE + 64`, "irq storm masked"). Increments the arch limiter's trip counter on a trip (replaces the caller-side `note_masked()`)."
   - `void storm_mask_line(uint8_t vector);` — "arch-effective mask for a tripped line: aarch64/riscv64 mask the GIC/PLIC source; x86 masks the PIC mirror line **and** the live I/O APIC entry when the APIC is enabled (the `mask_on_delivery` twin). Out-of-window/non-line vectors are a no-op (fail closed)."
   - `uint64_t storm_trips();` — diagnostic + regression-test delta.
   - `void storm_reset();` — burst-state reset only; `trips_` stays monotonic (documented; tests assert deltas).

**Phase P2 — off-x86 wiring (behavior-identical)**

2. `arch/aarch64/interrupt_controller.cpp`: keep `static IrqStormLimiter g_storm{}` (:35). Add the four functions next to it: `storm_gate` = `if (!g_storm.observe(intid, now)) return false; g_storm.note_masked(); return true;`; `storm_mask_line` = `ArchInterruptController::mask(vector)` (guard `vector < 32 || vector >= GIC_MAX_INTID` → return); `storm_trips` = `g_storm.masked_count()`; `storm_reset` = `g_storm.reset()`.
3. Same file, `handle_gic_irq` (:258-275): replace `if (g_storm.observe(intid, arch::Timer::ticks())) { ArchInterruptController::mask(...); g_storm.note_masked(); dmesg...}` with `if (arch::storm_gate(intid, arch::Timer::ticks())) { arch::storm_mask_line(static_cast<uint8_t>(intid)); dmesg... }`. Argument values, log code and the `else` dispatch branch unchanged.
4. `arch/riscv64/interrupt_controller.cpp`: identical substitution — accessors at :30, dispatch branch :148-165 (`storm_mask_line(static_cast<uint8_t>(intid))`, `plic_complete_s` tail untouched).

**Phase P3 — x86 gate (new TU) + dispatch wiring**

5. Create `src/kernel/arch/x86_64/irq_storm.cpp` with the GPL header + `/// @file`/`/// @brief` block:
   - includes: `<kernel/arch/hal/irq_storm.hpp>`, `<kernel/arch/hal/irq_vectors.hpp>`, `<kernel/arch/hal/idt.hpp>`, `<kernel/arch/x86_64/hal/apic.hpp>`, `<kernel/arch/hal/interrupt_controller.hpp>`, `<kernel/nexios_config.h>`.
   - `namespace { constexpr uint8_t kStormVectorMin = 33; constexpr uint8_t kStormVectorMax = 47; arch::IrqStormLimiter g_storm{}; static_assert(kStormVectorMin == arch::irq_cap_vector_min()); static_assert(kStormVectorMax == arch::irq_cap_vector_max()); }` — the `static_assert`s make the window drift-proof against `irq_vectors.hpp` (§1 freeze: 33–47 = IRQ1–15).
   - `bool storm_gate(uint64_t vector, uint64_t now_tick)`: **(a)** named exemption first — `if (vector == static_cast<uint64_t>(arch::InterruptVector::TIMER) || vector == static_cast<uint64_t>(arch::APIC::APIC_TIMER_VECTOR)) return false;` with a comment naming both tick sources (INV-2, Q8); **(b)** window fail-closed — `if (vector < kStormVectorMin || vector > kStormVectorMax) return false;` (INV-4: MSI-X/IPI/spurious never counted, no `vector - 32` on non-line vectors); **(c)** `if (!g_storm.observe(vector, now_tick)) return false; g_storm.note_masked(); return true;`.
   - `void storm_mask_line(uint8_t vector)`: re-check the window (fail-closed no-op outside it); `const uint8_t irq_line = vector - kStormVectorMin + 1U;` → equivalently `vector - 32U` (state the mapping in a comment); `arch::ArchInterruptController::mask(irq_line);` then `if (arch::APIC::is_enabled()) arch::APIC::mask_irq(irq_line, true);` — the exact two lines of `irq_delivery.cpp:122-128` (INV-3).
   - `uint64_t storm_trips()`, `void storm_reset()` — thin wrappers.
6. `kernel.cpp` — add `#include <kernel/arch/hal/irq_storm.hpp>` under the existing arch includes; in `handle_interrupt_c`, immediately **after** the `if (vector == 0x80) { … return; }` block (:2708-2731) and **before** the `#if defined(CONFIG_ARCH_X86_64) && CONFIG_CAP_MAX_IRQ` block (:2733):
   ```
   bool storm_tripped = false;
   #if defined(CONFIG_ARCH_X86_64)
       storm_tripped = arch::storm_gate(vector, arch::Timer::ticks());
       if (storm_tripped) {
           arch::storm_mask_line(static_cast<uint8_t>(vector));
           kernel::log::dmesg_push_sev(kernel::log::ErrorSubsystem::BASE,
                                       kernel::log::kDmesgBase_BASE + 64,
                                       kernel::log::LogSeverity::ERROR,
                                       "irq storm masked", vector);
       }
   #endif
   ```
   then wrap the three dispatch statements (`IrqDelivery::isr_entry` :2738-2739, the threaded-IRQ block :2745-2752, `arch::IDT::handle_interrupt` :2755) in one `if (!storm_tripped) { … }`. **Nothing after that** moves: APIC EOI, PIC EOI pair, TPR restore and the latency histogram stay unconditional (INV-5). `storm_tripped` is a used local on every arch; off-x86 it is a folded `false` (INV-12).
   - Reuse of `kDmesgBase_BASE + 64` is deliberate: it is the existing "irq storm masked" catalog code (verified unique to the two off-x86 sites); do **not** allocate a new code (dmesg catalog collision hazard).

**Phase P4 — `pending` saturation + overflow accounting**

7. `irq_delivery.hpp`: add near the mode/kind enums `/// @brief Saturation bound and overflow-accounting unit for IrqRegistration::pending.` `inline constexpr uint32_t kIrqPendingMax = 0xFFFFFFFFU;` (type bound, not a tunable — no `nexios_config.h` change). In `IrqRegistration` directly after `pending` (:105) add `uint32_t pending_overflow = 0; ///< increments refused by the pending cap (saturating; never wraps)`.
8. `irq_delivery.cpp` `isr_entry` (:471-475): replace `++r->pending;` with the saturating pair —
   `if (r->pending < kIrqPendingMax) { ++r->pending; } else if (r->pending_overflow < kIrqPendingMax) { ++r->pending_overflow; }`
   — no branch change to the WAIT_ACK mask call that follows. Document with one line: "INV-7: saturate and count; the delivery is *reported*, never silently dropped."
9. Reset pairing (INV-8) at all three sites: `claim_slot` (:309), `release_slot_idx` (:578), `drain_task` (:644) — `r->pending_overflow = 0;` (respect the surrounding indentation; the claim path has pre-existing drift at :298-305 — do not reformat it). Post-edit: `grep -n "pending = 0" src/kernel/irq_delivery.cpp` must show 3 hits, each paired (LEARNINGS repeated-pattern rule).
10. Leave the three decrease sites (`syscall_handlers_irq.cpp:221,311,316`) untouched — they are already `> 0`-guarded and saturation makes them exact.

**Phase P5 — single-waiter typed refusal**

11. `syscall/syscall_errors.hpp`: add an IRQ range after the VSPACE block (`1112`) —
    `/* IRQ slot errors (1200+, F8 #561) */` `X(IRQ_ALREADY_WAITING, 1201, "IRQ slot already has a waiter")`.
    `error_string` is X-macro generated (no second edit). No userspace mirror exists (verified: no `SYS_ERR_` in any `*.h` outside `syscall_errors.hpp`), so this is a purely additive internal code.
12. `syscall_handlers_irq.cpp` `sys_irq_wait`: inside the locked critical section, immediately after the revalidation block (:214-219) and **before** `IrqDelivery::unmask_for_wait(*r)` (:220), insert
    ```
    // INV-9 single waiter (hardening, spec 8.3(1): a second distinct waiter
    // cannot register today because recipient must match the caller; this
    // makes the invariant explicit and typed, and never unmasks the line.
    if (r->waiter != nullptr && r->waiter != t) {
        obj->release();
        return static_cast<uint64_t>(errors::SYS_ERR_IRQ_ALREADY_WAITING);
    }
    ```
    Keep the pre-lock fast path (:171) as-is (`GENERIC_FAIL` — not in F8 scope; note as S3).
13. `irq_delivery.hpp` class doc: state the invariant — "a slot has at most one waiter; a second distinct waiter is refused with `SYS_ERR_IRQ_ALREADY_WAITING` under the slot lock and never unmasks the line."

**Phase P6 — spec/doc sync + gates**

14. `docs/specs/microkernel-transition.md`: §7.3:468 → `CONFIG_IRQ_STORM_LIMIT | 256 (all three arches; x86 PIC lines 33–47)`; §8.3(1):538-550 → limiter **present on all three arches** (x86: PIC lines 33–47 via `handle_interrupt_c`, tick vectors 32/0xE0 exempt by name, mask = PIC mirror + I/O APIC entry); §11:765 → `present` for x86_64 with the F8 pointer; leave §11:766 (mask-on-delivery) **unchanged** (see the level-mask decision below).
15. Level-mask decision (no code): §11:766 already states the post-#547 truth — x86 masks on delivery in WAIT_ACK only, WAIT mode programs a level I/O APIC entry. After P3 the x86 WAIT-mode level line is bounded by the limiter, i.e. **exactly aarch64's contract**. Do **not** add mask-on-delivery to x86 WAIT mode: it would (a) diverge from aarch64, (b) change `pending` accumulation semantics for WAIT users (a masked level line can never queue a second pending), (c) alter the WAIT/WAIT_ACK mode split. Record this as an explicit verified no-change in the commit message and in §8.3.
16. Gates in order (each run appends its `test-history.txt` row; `tools/meson/build.sh --arch <arch>` first, fail closed):
    - builds: x86_64, aarch64, riscv64 (debug), then x86_64 release;
    - x86_64 debug: `cap_irq`, `cap_irq_notify`, `hal`, `capability`, `drivers`, `ipc`, `core_sched`, `storage`, `selftest` (CI gate), `bench_microkernel` (stub home — must stay 12/12), then `scripts/run_all_classes.sh x86_64 debug` once before handing to audit;
    - aarch64 debug: `hal`, `cap_irq`, `cap_irq_notify`, `capability`, `drivers`, `ipc`, `storage`;
    - riscv64 debug: `arch_riscv64`, `hal`, `cap_irq`, `cap_irq_notify`, `capability`, `drivers`, `storage`;
    - x86_64 release: `selftest` + `capability`;
    - off-x86 byte-identity evidence (INV-12): per-function objdump diff of `handle_gic_irq` and `handle_plic_trap` before/after (LEARNINGS: totals mislead; per-function is the record). Expected: identical, or a single inlined `storm_gate` body; any other change is a defect.
17. SIL 3 audit (Phase 3) on `git diff main -- <the files above>`; post report path + DECISION on #561.

## TEST STRATEGY

Test sanctity applies: all of the below is **QE-owned, developed on `testbed`**, merged to `main`; the developer writes no test file and does not edit the two existing stubs. Sequencing: the two IRQ stubs already exist (`test_microkernel_transition.cpp:244`, `:257`), so the test-first premise of PROMPT-dev §3 is satisfied for IRQ-1/IRQ-2; the waiter stub is added by QE.

**New tests (QE sketch — replace stubs with real asserts, then add the new ones)**

1. `hal_irq_storm_gate_bounded` — class `hal` (`test_hal.cpp`, runs on all three arches), +1 per arch. Doc-block:
   ```
   // Runmode: kernel
   // Testidea: the per-tick same-INTID storm gate bounds a stuck line and the
   //           x86 tick vectors are exempt by name (IRQ-1, spec 8.3(5)).
   // Input: local IrqStormLimiter unit run; live arch::storm_gate on a
   //        line-kind vector with the controller snapshot saved.
   // Expect: no trip at CONFIG_IRQ_STORM_LIMIT dispatches, trip at +1 and
   //         thereafter; a new tick value and a different INTID reset the
   //         burst; storm_mask_line clears line_enabled(); on x86 32 and 0xE0
   //         never trip over 4x the limit; tracker delta 0.
   // Depends: arch::IrqStormLimiter, arch::storm_gate/storm_mask_line/trips,
   //          ArchInterruptController snapshot/restore, APIC::ioapic_masked
   ```
   Pseudocode: snapshot controller (`IrqState`) + I/O APIC entry for the chosen line; assert `IrqDelivery::find(vector) == nullptr` (never fight the delivery table); `storm_reset()`; `trips0 = storm_trips()`; loop `i < CONFIG_IRQ_STORM_LIMIT` with a **fixed** tick value → all `false`, `line_enabled()==true`, `trips==trips0`; one more call at the same tick → `true`; `storm_mask_line(vector)` → `line_enabled()==false` and (x86) `APIC::ioapic_masked(line)==true`; `trips == trips0 + 1`; `storm_gate(vector, tick+1)` → `false`; `storm_gate(vector+1, tick)` → `false`; `#if defined(CONFIG_ARCH_X86_64)` leg: `storm_reset()`, then `2*CONFIG_IRQ_STORM_LIMIT` calls each for `arch::InterruptVector::TIMER` (32) and `arch::APIC::APIC_TIMER_VECTOR` (0xE0) → `storm_trips()` unchanged; teardown: `storm_reset()`, `ArchInterruptController::restore(saved)`, `APIC::ioapic_write_entry(line, saved_entry)`. Bounded loops only (≤ 2·256 iterations, `arch::pause()` every 32).
2. `cap_irq_pending_saturates` — class `cap_irq` (in the `capability` aggregate on all three arches), +1 per arch, replaces stub `irq_pending_capped` semantics:
   Pseudocode: create+arm an IrqCap (existing fixtures at `test_cap_irq.cpp:602` show the harness-driven `IrqDelivery::isr_entry` pattern); `IrqRegistration *r = IrqDelivery::find(vector)`; set `r->pending = kIrqPendingMax - 1`; `isr_entry` → `pending == kIrqPendingMax`, `pending_overflow == 0`; `isr_entry` ×3 → `pending` still `kIrqPendingMax` (never wraps), `pending_overflow == 3`; consume once via a pending-returning wait or by asserting the field after the harness path; teardown: set `pending = pending_overflow = 0` and release the slot, assert `occupied_count()==0`.
3. `cap_irq_waiter_second_refused` — class `cap_irq`, +1 per arch, invokes `sys_irq_wait` directly (kernel-side call with a real cap handle, as other sys_* fixtures do):
   Leg A (reachable, existing behavior): a **foreign** task's wait on the slot returns `SYS_ERR_GENERIC_FAIL` (recipient mismatch at :171) — assert first, it is the reason the new check is hardening.
   Leg B (the new invariant, white-box): plant `r->waiter` = a **live, non-blocked** TCB (spawn a minimal peer with the existing `task_ptr.hpp` fixtures; the pointer is only compared, never dereferenced) and `r->waiter_gen` = its generation; call `sys_irq_wait` from the test task → `SYS_ERR_IRQ_ALREADY_WAITING`; assert the line was **not** unmasked (WAIT_ACK leg) and `occupied_count()` unchanged; teardown clears `waiter/waiter_gen`, reaps the peer (tracker delta 0).
   Both legs under `test_isolate` snapshot/restore with `ResourceTracker::check` delta 0.
4. Retire/keep decision for the two stubs: leave `irq_storm_x86_bounded` and `irq_pending_capped` registered as stubs in `bench_microkernel` (12/12) — that class is not in any gate arch list and the real coverage lands in `hal`/`cap_irq` (in the `capability` aggregate, all three arches). Record the mapping so the M8 second half (#192 `irq_mux`) retires them.

**`test_expected_counts.hpp` bumps (QE):** `hal` 130/83/85 → 131/84/86; `cap_irq` 17/15/15 → 19/17/17; `capability` 161/0/0 → 163/0/0 (aggregate includes `cap_irq`). No change to `bench_microkernel` (12) or `drivers`.

**Existing class list to run (rationale):** `capability` + `drivers` are the M8 gate classes (§14:866) — `capability` carries `cap_irq`/`cap_irq_notify`/`cap_msix` (delivery-table regression surface: slot reuse, revoke/drain, WAIT_ACK unmask order); `drivers` exercises WAIT_ACK/MSI-X delivery end to end (virtio-blk request path); `hal` carries the new storm test + `test_hal` mask/snapshot tests; `ipc`/`core_sched` for ISR nesting, tick liveness and deferred-switch behaviour after the `handle_interrupt_c` restructure; `storage` for the block IRQ path; `selftest` as the CI gate. Probe-order note: run `cap_irq` (fast, ~340 ms measured) before `capability` (`capability` is the aggregate).

## SIL 3 / SAFETY RISKS

- [S1] Masking the wrong PIC line from a MSI-X/IPI/spurious vector (`vector - 32` on a vector ≥ 48 disables an unrelated device) — mitigation: gate returns `false` outside 33–47 **and** `storm_mask_line` re-checks the window as a fail-closed no-op (INV-4, defense in depth); the subtraction is reachable only on a trip.
- [S1] Tick vector masked ⇒ no scheduler tick ⇒ system-wide wedge — mitigation: the exemption is named first in `storm_gate` (32 and 0xE0, INV-2) *and* the window excludes 32; QE leg asserts 2×256 feeds of both never trip.
- [S1] Suppressed dispatch accidentally skips EOI/TPR restore (APIC ISR stuck, "no further interrupts" — the #252 wedge class) — mitigation: the `if (!storm_tripped)` wrap covers only the three dispatch statements; EOI/TPR/histogram stay unconditional (INV-5); audited by reading the block boundaries, which are fixed anchors (:2733/:2742/:2755/:2757).
- [S1] A storm-masked line behind an armed WAIT slot leaves the driver blocked with no wake — mitigation: unchanged from aarch64 today (limiter owns only the mask; `sys_irq_wait`'s optional tick bound returns `GENERIC_FAIL` and the driver falls back to bounded polling); documented in §8.3 as the shared, parity-identical consequence rather than fixed here.
- [S2] `pending` wrap (the F8 defect) makes a disabled/behind consumer see 0 after 2^32 deliveries — fixed by the saturating pair + overflow counter (INV-7).
- [S2] `pending`/`pending_overflow` reset drift across the three reset sites yields a handed-out slot with a stale overflow count — mitigation: reset-as-a-pair step + `grep` verification of all `pending = 0` sites (LEARNINGS repeated-pattern rule, auditor S2 precedent #307).
- [S2] Single-waiter check placed after `unmask_for_wait` would let a **refused** caller unmask a line the owner masked — mitigation: refusal is specified *before* the unmask (:220) and the test asserts the line stays masked.
- [S2] Off-x86 ISR codegen drift from substituting `storm_gate` (a stray semantic change hidden in a "refactor") — mitigation: same object, same order, trip counter moved inside the gate (delta-equal), plus the per-function objdump record and the aarch64/riscv64 class gates (INV-12).
- [S3] MSI-X floods (vectors 48–255) remain unbounded on x86 — deliberate: message-signalled delivery has no line to mask and masking an entry needs the owning cap (no re-arm path); Q8 puts MSI-X flood control at the wrong layer (owner decision 917). Documented in §8.3, follow-up candidate only.
- [S3] Single-slot A/B alternation blind spot (two storming lines interleaved never trip `last_intid_`) — unchanged and explicitly **follow-up-only** per Q8; not "fixed" here.
- [S3] New public seam (`storm_gate`/`storm_mask_line`/`storm_trips`/`storm_reset`) in a production header — precedent: `IrqDelivery::wait_timeout_fire` is public "for the stale-fire regression test"; the seam is allocation-free, lock-free, and its production caller is the ISR itself (no test-only path in production code).
- [S3] Pre-lock recipient mismatch still returns `SYS_ERR_GENERIC_FAIL` (:171) while the sibling refusal is now typed — CODING_STYLE §5.3 gap; deliberately out of F8 scope (ABI-visible behavior change), recorded for the #192/irq-mux pass.
- [S3] `r->lock_` remains an unranked lock; contract §2 defers its ranking to the M8 x86-hardening re-audit (#192 second half) — this change takes no new lock and does not worsen it.
- [S3] `nexios_config.h` untouched by design; if a future bound is needed it belongs in `cap/` (Q3/F9), not here.

## OPEN QUESTIONS

none — every design choice above is decided by spec §8.3/§11/§14 and the owner decision Q8 (§16:917). Two decisions are recorded here rather than escalated: (a) the MSI-X window (48–255) stays outside the gate (Q8 layer argument, S3 follow-up); (b) x86 WAIT-mode level lines are **not** given mask-on-delivery — the limiter is the parity mechanism, so §11:766 stays as written.
