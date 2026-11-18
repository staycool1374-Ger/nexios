# #582 fix spec vs microkernel-transition spec — compatibility record

**Date:** 2026-11-18. **Author:** dev lane. **Basis:** audits/plan-582-final.md (APPROVED, r6) vs docs/specs/microkernel-transition.md (main @ 7381dde7b).
**Purpose:** transition-spec traceability for the #582 implementation (Phase 2, in progress on main). Not an audit verdict; no code changes.

**Verdict: compatible, no blockers.** The fix enforces what the transition spec already demands and touches nothing it freezes. Four interface points need naming (two doc follow-ups, one sequencing note, one Q6 scoping note).

## Aligned (no action)

| # | Transition spec | #582 plan | Reading |
|---|---|---|---|
| 1 | M3 direct-switch on Call/REPLY_RECV only, single publisher, refusal → tick path (§5.6, F3, Q3) | No new switch paths; `try_direct_switch` preconditions unchanged; deny = defer-and-rearm, arm applied at most once | Scope intact. Deferral is a third outcome (refuse→tick, deny→retry), preserving at-most-once |
| 2 | Structural gate `slope_ticks_per_hop < 1.0`, per-hop bound 3 ticks (Q3, §5.6) | Fast path cost = 2 atomic loads on arm-carrying ISRs only; GATE 3 re-runs the `bench` slope gate at 1.00 tick/hop | The fix is what *protects* the M3 gate — the freeze it removes is worse than any slope violation |
| 3 | INV-MK-5 (switch applied only at depth-1/depth-2/nested-self-trigger epilogues) | Gate lives *inside* those same three epilogues; only adds a deny decision, no new vectors, no new apply sites | The *where* is untouched; only the *whether* is gated |
| 4 | §6.2 contract items 2+4 (no spinlock across `reschedule()`/BLOCKED; no deschedule while holding non-PI lock) | Apply gate = enforcement of item 4 for `scheduler_lock_`; new SCHED+16 diagnostic sits at the same `reschedule()` entry the contract already instruments | The fix is the enforcement mechanism §6.2 demanded; INV-MK-4 stays VIOLATED until this lands, then contributes toward clearing it |
| 5 | INV-MK-10 (3-arch parity) | Per-arch branch widenings + 3-arch gates + per-arch audit parity checks | Aligned by construction |
| 6 | Q11 single-core scope | Per-CPU reasoning, UP bounds; `smp.cpp` change mechanical only; no new cross-core protocol | No interaction |
| 7 | M3 gate classes (`ipc`, `bench`, §14) | #582 gates are a scheduler-side superset; slope gate shared with M3's | Consistent; no gate redefinition |
| 8 | No SEND-side switch without new evidence (§5.6); frozen syscall surface (T-1/Q3) | No new switch paths, no syscall changes | Untouched |
| 9 | §8.1 "keep core"; donation at M13 | Scheduler core kept; no SchedContext touched; boost path semantics preserved | No interference with M13 |

## Needs naming (4 items)

**1. Latency model refinement (§5.6: "loss degrades to one tick — never a strand").**
The fix makes loss degrade to *up to 8 deferred ticks, then fail-closed halt* — never a strand, which satisfies the sentence strictly, but exceeds the 1-tick figure and Q3's frozen 3-tick per-hop bound in the anomaly window. Mitigation is in-plan (legitimate holds are µs-scale so deferral ≈ 0 in health; bench slope gate; FailClosed is loud, not silent). Still, §5.6's sentence is now imprecise. *Follow-up: one-line pointer in §5.6 to the scheduler.md apply-gate section (normative-spec edit → owner sign-off, file separately, not in the dev lane).*

**2. Sequencing vs M4.**
Appendix A shows M4 (mem-mgr bootstrap) uncommitted/in-progress, gated on `core_memory` + `capability` — classes that never exercise the relay chain, so the freeze can flake M4's soak silently. *Recommendation: land #582 before or alongside M4, and add an Appendix A row for it when committed (non-normative, one line, keeps the map complete).*

**3. Q6 "zero fallback hits" vs GATE 1's "≥1 APPLY-DEFER per boot".**
Q6's closed list demands zero fallback hits, while the #582 gate *requires* ≥1 deferral per soak boot as proof the gate executes. These don't actually conflict — deferral is an internal retry invisible to the functional path (arm lands next tick), same class as the designed tick-path refusal — but if QE applies Q6 literally to the #582 merge, it reads as a contradiction. *Recommendation: scope Q6's zero-fallback criterion to flips; count deferral as designed-retry like tick-path refusal.*

**4. Detector gating record (already in-plan, restated for the transition-spec reader).**
The IrqGuard-freeze window has log/latch-only coverage by design ([STALL] HALT is `CONFIG_DEBUG_IPC_SCHED`-gated, tick_watchdog never panics). This matches the transition spec's posture (no release halt assumed anywhere), but anyone reading §6.2 item 6 ("tick watchdog SHALL detect a stalled tick") should not mistake the #582 detector set for a release halt. The plan already records this — no action beyond not claiming more.
