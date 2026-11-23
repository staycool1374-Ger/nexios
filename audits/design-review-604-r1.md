# AUDIT REPORT 2026-11-22T12-56-11Z
PATCH: audits/plan-604.md (SIL 3 DESIGN review — no code diff exists yet)
FILES: audits/plan-604.md
BASE: main @ 16c040705 (verified `git log --oneline -1` = `16c040705 Harness: QEMU 8.2.2-compatible KVM fallback list on x86_64`)

## VERIFIED OK (spot-checked against sources)
- Base, D1, D2: tag `archive/ipc-sched-trace-kit` exists locally AND on origin, peeled target = 16c040705, content = intact kit (`#define` commented, BUF macro, h2 decl); #608 open (S2, detection-only, witness + base-rate recorded).
- Item 1 dead-code claim: macro undefined everywhere in tracked tree (header:34 commented out; no `nexios_config.h` entry; no build-system `-D`; whole-tree grep shows only the listed `#if defined` sites + test/history mentions). Every listed deletion block opens with the gate; tick.cpp:330 `[WEDGE]` sits inside the 183–399 block (no missing call site); ipc.cpp 4 sites match; global_state [H2-DEAD] 1065–1077 is logging-only (`#endif` precedes `drop_arm`, verified).
- H2_REC unconditional-noop is the true zero-behaviour choice (re-gating on CONFIG_DEBUG would arm the ring — behaviour change, correctly refused).
- Item 4a mapping complete: 11 struct fields ↔ 11 distinct accessors; suspected reset gap DISPROVEN — `runq_desync_reset_for_test` (registry.cpp:430-437) zeroes 5 counters, so the 7-reset composition covers all 11. No new storage/lock/writer.
- Items 2/3 zero-eligible: criterion is sound conservatism; spot-verified tick.cpp:692 (functional sweep, zero test asserts on `ipc_wake_rescue` in `src/kernel/test/`) and park_lock.cpp:148 (kill-switch inside fence region (1) — KEEP correct). Net reduction still flows from item 1.
- Fences avoided: deletions (655–834, 1065–1074) clear 889–989 / 1152–1250 / 1354–1404, park_lock 147–198, rqm 121–174.
- #608: RED/NOT ELIGIBLE + filed + tripwire fully live, no weakening anywhere.
- Sanctity/QE hand-off precise (stub-first, counts-row-before-assertions, 4b BLOCKED-BY); per-commit gates + 6-build matrix + objdump identity + C0 retrieval artifacts (graphify ×3 with dispositions, obsidian 0-match ignored with reason) all present.
- 5-doc C1c closure list is the right set; check-config tombstone precedent (L314-319) exists as cited.

## FINDINGS
- [S2] audits/plan-604.md:63 — step-12 grep-zero gate unsatisfiable as written and contradicts test sanctity
  WHY: `test_jitter.cpp:25,99` guards + `test_kernel_top.cpp:837` comment will still hit, but no test file is touched and `src/kernel/test/**` is NOT-touched, while only §13 + tombstone are exempt.
- [S2] audits/plan-604.md:96 — fence shorthand `global_state.cpp:889-1250` overbroad, contradicts §INVARIANTS narrow fences
  WHY: it false-REJECTs the plan's own [H2-DEAD] deletion at 1065–1074, which sits in the 990–1151 gap outside the true fences (889–989, 1152–1250).
- [S3] audits/plan-604.md:62 — holder-table arithmetic cites stale 36/34; doc:244 reads 35/33
  WHY: correct target is 35→33 total with release unchanged 33→33 (release never contained the gated pair) and adopting 7(5)→5(5); anchors also drifted (:86,:105 → ~:101,:120); dev must recount.
- [S3] audits/plan-604.md:60 — h2_record decl un-gating unspecified; include inventory undercounts (8 includers, incl. unconditional `fmt_u64` user `tcb_write_log.cpp`)
  WHY: decl lives inside the L119 gate being deleted, and `scheduler_debug.cpp:186-187` ("declared … under CONFIG_DEBUG_IPC_SCHED") goes stale either way.
- [S3] audits/plan-604.md:70 — C4 no-new-writer grep misses `store`/`exchange`
  WHY: a store added to the read path would pass `^\+.*(\+\+|fetch_add|fetch_sub)`; strengthen to flag `store|exchange` outside `reset_sched_oracles_for_test`.
- [S3] audits/plan-604.md C5 file list — omits `docs/specs/microkernel-transition.md` (#604 unrecorded; complexity-budget rule lives only in chat/issue)
  WHY: add F-table row F22 (#604, F11-gate, task/sync scope, F21 precedent) + §16 Q-row for the standing owner-decided budget rule; M-table and §8.8 untouched; C5 placement not C7 so the audit sees the provenance.
- [S3] audits/plan-604.md step 16 — `nexios_config.h:1205` stale-comment "fix the comment here" scope ambiguous vs C5 docs-only
  WHY: clarify note-only-in-table vs explicit config-header carve-out before C5 lands.

## PATCH
`audits/rejected_patch.diff` written (2 hunks: step-12 criterion scoping + test/history exemptions; fence shorthand narrowed to 889–989/1152–1250 with doc-lie cross-ref). S3 items are prose for the plan revision.

DECISION: REJECTED
