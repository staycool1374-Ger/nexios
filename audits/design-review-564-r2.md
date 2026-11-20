# AUDIT REPORT design-review-564-r2 (SIL 3 design audit, evidentiary standard)
PLAN: audits/plan-564.md (rev 2, base main tip 2275e89c8 — tip re-verified via prior r1 audit; file untracked, read directly)
FILES: plan-564.md only (design audit; no code patch; NOTHING committed)

## Revision-history check
- Rev-2 row present (plan line 8): scopes all seven r1 fixes. PASS.

## Fix verification (each checked against plan text, not asserted)

1. S1 Q9 single-gate (r1: PD-held CREATOR cap confers zero stop power; test mis-scoped) — FIXED, real:
   - INV-PROC-6 (line 63) now states PD NEVER calls `tcb_control_authorize` as caller; cites branch 5 (`tcb_control.cpp:139-145`, `parent_id == caller.id`, no cap consulted); acknowledges Stage B fork clones in invoking-client context so parent_id names the client; states a proc-mgr-as-caller stop would fail branch 5 and branch 6 needs SUPERVISOR scope which is forbidden; held CREATOR caps are lifecycle-tracking holds, "not stop power"; no second predicate; SUPERVISOR stays kernel-compose-only.
   - kill dispatch (line 94) rescoped: disposition verdict from proxy, mechanism + gate call in invoking-client context, "never a proc-mgr-as-caller gate call (INV-PROC-6)".
   - `proc_mgr_stop_authority` sketch (line 113) rescoped to CLIENT callers (own-child stop + 5 typed refusals); "PD-as-caller path is explicitly NOT tested (it cannot pass the unchanged gate by construction)".
   - No residual claim that a PD-held CREATOR cap passes the gate (line 134 mitigation restates "lifecycle tracking only"). Line 48 "proc-mgr's stop authority is the CREATOR branch (Q9 client subtree)" is read in context as subtree-scoped client-context stops, disambiguated by INV-PROC-6. PASS.

2. S2 funnel lock set (r1: lock-free edge-drop unimplementable; scheduler_lock_ premise inverted) — FIXED, real:
   - INV-PROC-5 (line 62): derivation leaf + holder-CNode leaf under IrqGuard, `scheduler_lock_depth_own()==0` cited (`scheduler_terminate.cpp:54`, release-before-publish `:172-177`), never scheduler_lock_; same leaf order as hold()/revoke; idempotent double-publish (prompt + `cleanup()` backstop `task.cpp:2862-2868`), generation-mismatch no-op.
   - Step 6 (line 78) and funnel hook step 7 (line 79) restate the same lock set (IrqGuard, scheduler_lock_ NOT held, no logging).
   - Grep confirms zero remaining "may hold scheduler_lock_" in the file. PASS.

3. S2 Stage B spec violation (r1: flip ships violating §8.8 item 2) — FIXED, real:
   - Affected-files docs row (line 53) carries the explicit §8.8-item-2 INTERIM waiver ("Stage B retains clone deep copy; carve fork deferred to F11a"); step 17 (line 99) makes the debug-default flip ride on the waiver being in the tree (parity green AND waiver landed); release default stays 0 until the 2x3 re-run. The r1 either/or (waiver at Stage B, or hold at 0) is satisfied via the waiver leg. Q1 (line 145) keeps carve deferred to F11a. PASS.

4. S2 byte-identity / §8.10 ordering (r1: OFF-build binary necessarily differs; restart row must precede MOVE) — FIXED, real, with two S3-grade stale adjectives (OBS-1, OBS-2 below, non-blocking):
   - INV-PROC-7 (line 64) downgraded to behavior-identity with PROCFS precedent, explicitly disclaiming binary byte-identity (new PdRole, re-derived count, allowlists, asserts, ProcSvc wrappers enumerated as the necessary binary delta). OFF-build line (line 38) says behavior-identical. PASS on substance.
   - §8.10: SDF row is step 8 (Stage A); restart row is step 16 with an explicit "land BEFORE the dispatch MOVE (step 15) takes effect" clause gated by the step-17 flip (nothing takes effect before parity-green + waiver). The real ordering is enforced by the step-17 gate even though the step numbers run 15-then-16 (see OBS-2).

5. S3s — ALL FIXED, real:
   - Funnel wording: no "may hold scheduler_lock_" remains (grep-verified).
   - Hold-failure order (line 91): CAP_TABLE_FULL destroys the 33rd child PRE-publication — never reaches `publish_task_death`, no death record, waiters see only the typed refusal.
   - getpid writer (line 97): writer named as the fork/exec path, same cells the PD decision path maintains, no replica. PASS.

6. Regression (r1 sound items) — ALL INTACT:
   - Stage split A/B/C with C gated on Q1 (lines 71/86/101-103); Q1(a)/Q2(a)/Q3 positions unchanged (lines 145-147); death-path placement + (pid,gen) closure + tracker-delta-0 (lines 65, 79, 110-112); check-7 retrieval with dispositions (lines 14-23); arch parity (line 67); no false-close (§15 row stays `new`, Appendix A deferred, carve unproven — lines 53, 84). PASS.

## Non-blocking observations (S3, do not gate implementation)
- OBS-1: risk-table line 130 still cites "INV-PROC-7 (OFF build byte-identical)" — stale adjective contradicting the downgraded invariant on line 64. Normative text (line 64) governs; suggest "OFF-build behavior-identical" at next touch.
- OBS-2: restart row numbered step 16 sorts after dispatch MOVE step 15 while its text claims pre-MOVE landing. Substance holds via the step-17 flip gate (both rows land before anything takes effect); suggest renumbering 15/16 at next touch for literal §8.10 readability.
- OBS-3: "byte-identical bodies" (lines 41/76) and "stays byte-identical" (line 93) refer to source-statement identity of moved handler bodies (git-diff move purity), distinct from the disclaimed binary claim — acceptable, but "statement-identical" would avoid re-inviting the r1 confusion.

## Counts
- r1 findings re-verified fixed: 7/7 (1 S1 + 3 S2 + 3 S3).
- New findings: 0 (3 non-blocking S3 observations only).
- Residual S1/S2: 0.

DECISION: APPROVED
