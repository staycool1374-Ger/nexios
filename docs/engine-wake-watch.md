# Engine wake fast-path watch (#466 R4)

One quiet week of fast-path ticks with zero duplicate filings closes R4;
any duplicate filing opens a follow-up issue with the log excerpts —
never an inline patch (the window must stay observation-only).

## Window

- Start: the merge sha + engine restart time that first deploys #466 R1/R2.
  Record both here when the window opens (R1/R2 must be deployed first
  so the sample is not contaminated by pre-delivery code).
- Freeze for the window: `scripts/agent_up.sh` wake path and the
  demand-shaped predicate — no engine code changes until the tally.

## Why this watch exists

`fire_targeted_tick` (agent_up.sh) fires a detached one-shot
`tick --issues N` on every demand-shaped worker post
(`NEED ::`, `DECISION ::`, inline `DECISION:`, `DONE:`/`DONE `).
The sweep lock serializes it with the 180 s loop (skip-if-held), so
overlap is safe by construction. The known pressure point is the
shared-second edge: `coord_note` oids embed `role:ts19`, so two filers
in the same second share an oid ("order deferred ... (oid collision)"
is the visible symptom, seen 12:43:04Z). A same-second collision is
NOT a duplicate order — a duplicate is the same oid filed twice, or
two same-lane filings within 5 s for one demand.

## Metrics (per day)

1. Fast-path tick count: `fire_targeted_tick` invocations.
2. Lock skips: `cycle skipped: another engine tick holds the lock` lines
   (state_machine.py) — skips are the safety valve working, not loss.
3. `order filed` lines per (room, lane, oid) from engine-console.log.
4. `oid collision` lines (shared-second edge — expected rare, not a defect).
5. Duplicate candidates: same oid filed twice, or two same-lane filings
   within 5 s for one demand. Zero is the pass criterion.

## Log queries

Against `logs/state-machine/engine-console.log` and
`logs/state-machine/state-machine.log` (window lines only —
exclude everything before the recorded start time):

- Ticks fired: lines matching `fire_targeted_tick` / `tick --issues`
- Skips: `cycle skipped: another engine tick holds the lock`
- Filings: `order filed` (group by oid, lane, room)
- Collisions: `oid collision`
- Escalations (R1 cross-check): `DLR-OVERDUE`, `delivery ... ESCALATED`

## Evidence template (post as a #466 comment)

- Window: <start ts> → <end ts> (7+ days), merge sha <sha>.
- Fast-path ticks: <n>, lock skips: <n>, oid collisions: <n>.
- Duplicate filings: <0 | excerpts>.
- Verdict: close R4 (zero) / follow-up issue (nonzero, with excerpts
  and the deterministic-oid proposal — never patch inline).

## Decision rule

- Zero duplicate filings for 7 consecutive days → close R4 with the
  evidence comment above.
- Any duplicate → open a follow-up issue with the log excerpts and the
  deterministic-oid proposal; the window restarts after that fix.
