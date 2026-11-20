# DESIGN REVIEW — issue #589 (rv64 isr_nesting baseline normalization, owner option (a)) — R1

PLAN: audits/plan-589.md @ base main tip `3dc3f84da` (verified `git log --oneline -1`; branch `main`).
SCOPE: design audit only. No code diff exists; PROMPT-audit evidentiary standard applied to the plan's
cited sources, each re-read in-tree this session.

## PREMISE VERDICTS (the two overturns)

- OVERTURN 1 (SBI ecalls cannot increment) — SOUND. S-mode ecalls trap to M-mode via mtvec unless
  delegated; M-mode handling never enters the kernel stvec path (`syscall_entry.S:159-163` is the sole
  inc site). If delegated (medeleg[9]=1), the trap DOES enter stvec but pairs inc (`:160-163`) with the
  tail dec (`:462-478`), leaving no residual. Either delegation outcome leaves zero outstanding at a
  quiescent read. Citation staleness only (see S3-2): the pinning test is deregistered.
- OVERTURN 2 (PerCpu.isr_nesting_depth authoritative ONLY on x86_64) — SOUND. `cpu_context.hpp:90-105`
  branches: x86_64 reads `per_cpu[].isr_nesting_depth`, all other archs read the plain global.
  `global_state.cpp:123-126` defines the plain global only under `#if !defined(CONFIG_ARCH_X86_64)`.
  rv64 trap asm addresses the plain global by `la` (`:160/:253/:468`); aa64 vectors never touch any
  counter (sole hit is a comment, `vectors.S:297`). The 0x20 slots exist on all three archs
  (`percpu.hpp` ×3) and are zeroed by every `percpu_init_bsp` but read by nobody off-x86. F4 stands.
- `.Ltrap_park` (:546-555) — CONFIRMED the only non-decrementing exit path, and it halts (`wfi` loop),
  so it cannot accrue in a completing boot. The second park (`:199`, arm-published-but-null) likewise
  halts. Every other exit flows through the `:462-478` dec. The `:219-238` "accounting-sound" comment
  is sound for frames that eventually resume (each inc pairs with exactly one epilogue dec, including
  across applies); the actual leak shape is abandonment-without-resume (termination while holding
  frames), compounded by — and indistinguishable in this plan from — BY-DESIGN held frames (see S2-1).

## DESIGN CHECKS

4. Fix point — VERIFIED SAFE. Insertion is immediately after the percpu_init_bsp `#endif` (:1054), so
   nothing runs between percpu_init and the call. Everything that could trap or enable interrupts runs
   later: `IDT::init/load` (:1136-1137), `ArchInterruptController::init` (:1151), `Timer::init` (:1663),
   `smp::bring_up` (:1689-1693, HSM). Single-threaded (APs firmware-held until HSM bring-up). SIE==0 by
   OpenSBI entry state plus zero `sstatus`/`sie` writes in `boot.S` (grep: none) plus no `sti` before the
   scheduler idle-loop entry. Zero-outstanding-frames holds: any pre-site trap either balanced
   (inc→dec) or parked (boot would not continue); SBI ecalls are accounting-invisible either way.
   `ENSURE(!interrupts_enabled())` backstops it fail-closed (panic, both builds — `assert.hpp:48`).
5. One-shot — OVERCLAIMED (S3-1). `verify_and_write` BOOT_ONLY gates on `ctx.phase != BOOT` only
   (`global_state.hpp:130-133`); a second BOOT-phase write is ACCEPTED. The precedent
   `try_set_boot_epoch` (:217-220) is phase-gated, not one-shot. Practical safety rests on the single
   call site, not the primitive.
6. Verify-only x86_64/aa64 — ADEQUATE, no finding. Bodies are read-only (no store possible → no
   corruption vector); aa64 returns the degenerate permanent-0 with the F3 doc block already required
   by step 5 — observability, correctly not presented as a maintained invariant.
7. Tripwire — PLACEMENT adequate, INTERPRETATION unsound (S2-1). Read-only at `init_task_main` entry
   respects INV-NEST-1; `debug_write`/`debug_write_hex` (`kernel.cpp:811/816`) respect INV-NEST-6. BUT:
   (a) step 9 claims "nonzero proves post-handover accrual" — false: nonzero also reads live HELD
   frames, which are by-design (the plan's own :228-229 documents steady-state live depth 4 from
   "init's daemon-wait + several daemon recv-hlts"); (b) the value is dispatch-order-dependent:
   vfsd/iocd/watchdogd (prio 20, `taskdefs.cpp:302-314`) outrank init (prio 0, `:300`), so daemons
   trap-and-block holding frames before init's first dispatch on full boots — leg 3 fires by design;
   if sporadic-server budget delays a daemon, it fires 0 — nondeterministic acceptance input.
8. No-touch list — HONORED. No step edits `irq_guard.hpp`, `park_lock.cpp`, `scheduler_tick.cpp`, or
   any `*.S`. Step 11 touches docs only.
9. S3s/QE/gates — S3 spread reasonable (3×S1, 3×S2, 2×S3). QE sketch: 3 tests named, stuck-line N/A with
   reason, no count rows (correct: no in-dev tests per test sanctity). Gates cover 3-arch build +
   boot smoke + consumer classes. Two sketch defects (S2-2; S2-1 applied to QE-2) and two gate
   suggestions (S3-3, S3-4).
10. Scope — CLEAN. Boot-only single aligned store; no locks (correct: single-threaded, SIE==0);
    no config; no T-1; `__atomic_store_n` pre-scheduler harmless.

## LOAD-BEARING QUESTION

Does the step-13 rule genuinely discriminate present-at-handover vs accrues-after, and is
normalize-once-at-handover sufficient if accrual continues? NO on discrimination, YES-with-caveat on
sufficiency. The `[NEST]` leg measures handover-residual + daemons' held frames + genuine leaks as one
number with no census, so leg 3 ("task-entry != 0 → file balance follow-up") misfires on the expected
full-boot outcome. Normalization itself is safe and sufficient FOR handover-carry (evidence says the
handover reads 0 regardless); continued post-handover accrual (termination-leak) is untouched by the
write — which the plan admits by forbidding a "fixed" claim under leg 3. So the plan converges to
green CI + an honestly-open issue, but via a MISDIAGNOSIS by default: a balance hunt commissioned from
by-design evidence, and a permanent tripwire that fires normally. That breaks the plan's
evidence-instead-of-assumption core and must be fixed before approval.

## FINDINGS

- [S2] audits/plan-589.md step 9 + step-13 leg 3 (+ QE-2) — task-entry nonzero conflated with accrual.
  WHY: `taskdefs.cpp:300-314` (daemons prio 20 dispatch before init prio 0) plus documented live held
  frames (`syscall_entry.S:228-229`, live depth 4) make `[NEST] != 0` the expected full-boot outcome and
  dispatch-order-dependent; "proves post-handover accrual" is unsound and leg 3 misfires by design.
  FIX (in rejected_patch.diff): require a held-frame census (walk `all_tasks` states; BLOCKED
  user/daemon tasks holding frames exclude accrual) before filing the balance follow-up; reword
  "proves" to "indicates".
- [S2] audits/plan-589.md QE sketch test 1 — asserts `baseline == 0`, contradicting step-13 leg 1
  (handover != 0 is the "write IS the fix", close-eligible case, on which the assert can never pass).
  WHY: acceptance criterion and verdict rule cannot both hold on firmware-carry machines.
  FIX (in rejected_patch.diff): assert the stored baseline equals the step-13 recorded observed value
  (write happened exactly once), not `== 0`.
- [S3] audits/plan-589.md INV-NEST-2 — "one-shot enforced mechanically" overclaims: BOOT_ONLY gates
  phase, not count (`global_state.hpp:130-133`); a second BOOT-phase write succeeds. Safety rests on
  the single call site + post-boot rejection. Reword; no behavior change.
- [S3] audits/plan-589.md F1 citation — the medeleg pin (`test_riscv64.cpp:615-629`) is DEREGISTERED
  (`:1423-1426`, M-mode-only CSR, illegal-insn in S-mode). Premise holds a fortiori (S-mode cannot even
  observe medeleg) but "pins" overstates a non-running test. Cite the deregistration.
- [S3] Gates — add one release `none` smoke on rv64 (the handover `ENSURE` panics in release too) and
  consider `arch_x86_64` (new x86 symbol + boot-path print). Debug-only gates otherwise adequate since
  control flow has no CONFIG_DEBUG divergence.
- [S3] Step 3 `ENSURE(!interrupts_enabled())` panics the boot on exotic firmware entering S-mode with
  SIE==1; consider `cli()`-first (establish the precondition) + ENSURE as belt-and-braces. QEMU
  OpenSBI enters SIE==0, so gates are unaffected either way.

## PATCH

`audits/rejected_patch.diff` written: six hunks against `audits/plan-589.md` (F1 citation, INV-NEST-2
wording, step-9 tripwire semantics + census rule, step-13 leg-3 discrimination, QE-1 assertion,
QE-2 held-frame precondition). Apply verbatim (`git apply --check` first), re-verify, re-audit.

DECISION: REJECTED
