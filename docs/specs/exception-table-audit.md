# Exception Vector Table Audit — Modern x86_64 Vectors & Uniform Frames

**Doc ID:** NEX-SPEC-2026-08-23-006
**Status:** DRAFT (contains one CONCRETE BUG FIX, see §2)
**Milestone target:** v0.4.3 (bug fix could ride any v0.4.x release)
**Inspiration:** Cyjon `kernel/idt.asm` — full 32-vector discrimination with
per-vector error-code push so every frame is uniform.
**Related:** `src/kernel/arch/x86_64/isr_stubs.asm`, `docs/specs/drivers.md`
§6, `docs/specs/memory.md` (guard-page panic path), boundary spec signal
delivery.

## 1. Current State (verified)

isr_stubs.asm covers all 256 vectors: explicit ISR_NOERR/ISR_ERR for 0–31,
then `%rep 224` for 32–255, feeding a common `isr_common` (TSC capture, GPR
push, C dispatch). That structure is sound and matches Cyjon's uniform-frame
principle. drivers.md §6 documents the flow including IST1 for #DF and the
0xEE trap gate for syscall vector 0x80.

## 2. Concrete Bug: error-code classification of newer exceptions

Verified macro assignments include:

```asm
ISR_NOERR 20    ; #XM  SIMD floating-point — correct (no error code)
ISR_NOERR 21    ; ← WRONG on CPUs with VMX: #VE (Virtualization Exception)
                ;     DOES push an error code (EPT violation info)
ISR_NOERR 30    ; ← WRONG on CPUs with CET: #CP (Control-Protection
                ;     Exception) pushes an error code
ISR_ERR   17    ; #AC alignment check — correct (when CPL3 + AC)
```

Additionally relevant: #SX (vector 30 shares nothing — actually #SX is
vector 30 on Intel with CET shadow stacks... correction below) and
**#HV (vector 28, Hypervisor Injection)** which pushes an error code but is
rarely enumerated.

Precise table of vectors whose error-code behavior differs from legacy
assumptions:

| Vec | Name | Pushes err? | Our macro today | Fix |
|-----|------|-------------|-----------------|-----|
| 20 | #XM | no | NOERR | ok |
| 21 | #VE | **yes** | NOERR | → ERR |
| 28 | #HV | **yes** | NOERR | → ERR |
| 30 | #SX | no (shadow-stack) | NOERR | ok |
| 21+ | #CP | **yes** (vec 21 on AMD CET) | NOERR | → ERR |

Note the vendor split: #CP is vector 21 on AMD (CET), while Intel places
Control-Protection at 21 as well; #VE (Intel VMX/EPT) is also 21 — both
push an error code, so the fix is the same regardless of vendor. #HV is 28
(Intel hyper-V style injection). Net change: **vectors 21 and 28 become
ISR_ERR.**

Why it matters: with the wrong macro the CPU's pushed error code is
misinterpreted as the first GPR (RAX) — exception dispatch reads garbage,
guard-page/user-recover logic (drivers.md §6 `v<32` branch) acts on a wrong
frame, and `iretq` pops a misaligned frame ⇒ triple-fault-class failure on
any machine where these fire (nested-virt testing, CET-enabled hosts).
Latent today only because our test hardware never raises 21/28.

## 3. Hardening Adopted From Cyjon's Approach

1. **Compile-time frame-size assertion:** add `%assign` accounting inside
   the macros so NASM errors if the two classes ever produce different frame
   sizes (guards future edits like §2).
2. **Uniform dummy-push discipline** (already present): NOERR vectors push a
   synthetic 0 — keep, document next to the macros.
3. **Vector-name map for panics:** static string table 0–31 used by the
   panic path (`panic("EXC 14 #PF cr2=…")`) — cheap, removes hex-only
   triage. Cyjon's per-handler comments show how much this aids forensics;
   we get it without 32 separate handlers by indexing vec in isr_common.
4. **Reserved-vector policy:** vectors 15, 22–26, 29, 31 (reserved or
   vendor-specific) currently fall into generic handling; explicitly route
   them to a `panic("reserved exception")` branch instead of generic user-
   recover checks — firing at all means something is deeply wrong.

## 4. Test Plan

Class `exc_table`:
1. `err_macro_consistency` — assemble-time check (§3.1) plus runtime probe:
   trigger #UD(6)/#DE(0) in a test task and assert frame fields parse
   correctly through handle_interrupt_c.
2. `ve_cp_frame_layout` — QEMU with `-cpu host` nested options; inject EPT
   violation in a minimal guest OR unit-simulate: feed a synthetic frame
   shaped as CPU would for vec 21, assert dispatcher sees vec=21, err≠garbage
   RAX. (Full #VE/#CP live-fire is hardware-dependent; simulated-frame test
   is the mandatory gate.)
3. `reserved_vec_panics` — invoke reserved vector handler directly in test
   mode (latched, not fatal) and assert the reserved-branch was taken.
4. Regression: full IRQ/exc suite green; guard-page tests unchanged.

Validation: debug/release `all`, selftest, test-history rows.

## 5. Non-Goals

- IST redesign, FRED/event delivery, interrupt stack tables beyond the
  existing #DF IST1.
