# SIL 3 audit — issue #465 (ACK doctrine implementation + wake fast path + C1–C4 follow-ups)

Scope: `git diff 6fb4bd728..HEAD -- scripts/state_machine.py scripts/agent_up.sh prompts/PROMPT-{dev,testdev,audit}.md` (host tooling only, branch `main`); kernel C++ rules N/A by document scope — §2 naming / §8 formatting / §9 docs applied by analogy, §7 via selftests. Logic verified by selftest suite (509/509) plus live-tick evidence cited per item.

## FINDINGS

(none — no S1/S2/S3 violations.)

Checks, all pass:
- New identifiers (`escalate_ledger`, `verdict_identity`, `verdict_already_handed`, `newest_audit_done_post`, `tick_eta`, `fire_targeted_tick`, `ctx.tick_interval`) follow §2 snake_case, 4-space indent, docstrings on all new functions.
- Escalate branches (coord orders + resume-NEXT) preserve prior behavior exactly when `--allow-recreate` is off (NEED line + park at RING_ESCALATED); with the flag they kill + relaunch via the pre-existing gated `recreate_worker` (tip-pinned resume brief, KILL-UNCONFIRMED guard, MANUAL fallback). No new kill authority beyond the #464-audited auditor path — same function family, same guards. Live-verified: auditor session killed (tmux gone, record absent), no 4th ring anywhere.
- Suppression predicates are conservative in the safe direction: `audit_demand_answered` only returns True with demand_key match AND verdict present AND handled demand match AND no newer audit-routed DONE; `verdict_already_handed` only matches identical (kind, tip7, demand_key) triples, kind-flips always deliver. Live-verified both directions on room 390 (reopen fired) and room 240 (idle DONE correctly ignored).
- Status-window fix scans from the last POST line; `last_state_next` semantics unchanged (last match wins). Live-verified: NEXT "waiting auditor plan-review" parsed, demand filed same sweep.
- ETA heartbeat is additive (`tick_utc`/`interval_s` in graph JSON, reader degrades to "unknown"); one-shot ticks preserve the recorded interval instead of clobbering it with 0. Selftest covers the preserve path implicitly via `tick_eta` absence-of-crash; `escalate_ledger`, `verdict_identity`, `verdict_already_handed`, `newest_audit_done_post` (both polarities), inline verdict ts, and post-block parse all have explicit checks.
- `agent_up.sh` fast path: gate regexes verified against live inbox lines (fires on NEED/DECISION/DONE, quiet on ACK/status); skip-if-held proven live (rc 0, zero side effects); `comms-test` passes; `bash -n` clean. One-shot inherits the daemon's lock discipline — no new concurrency surface.
- WIRE block byte-identical across the three role prompts (5 lines).

S3 process note (non-blocking): three same-second orders share one oid (`dev:<ts>` keys on role+second); per-room ledgers keep them separate and dev ACKs per room box, but a sub-second filer should add a sequence suffix. Left as-is: cosmetic, no misdelivery observed.

## EVIDENCE

- Live ticks #442–#450: 390 demand → relaunch rc=0 → APPROVED → handoff → dev IMPL + QE engaged; duplicate suppressed (no re-handoff); session absent post-teardown.
- Selftest: 509/509 at commit.
- Gate: `check-close` requires this DECISION comment; `check-feedback` follows post-close with the #465 LEARNINGS entry.

DECISION: APPROVED
