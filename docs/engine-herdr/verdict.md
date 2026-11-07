# T9 verdict — herdr-core engine experiment (issue #507)

Date: 2026-11-07. Pilot room: #420 (test colocation). Branch: `engine-herdr`.
Corpus at verdict: 164+ tests green across herdr_engine (shim, transport,
FSM, auditor, scheduler, escalate, cli).

## Verdict: CONDITIONAL PASS

The room walked RECON → IMPL → VERIFY → CLOSE-OUT on typed posts alone,
and #420 closed as completed with the work committed (`pilot/420-colocation`)
and pushed. The acceptance unit from #467 ("a room walks DONE→CLOSED end to
end — never walked once") has now been walked once.

## What the success criteria say, honestly

1. **DONE→CLOSED walk: PASS with a caveat.** Every phase transition was
   gated on schema-validated typed posts; zero human-authored posts exist in
   the stream (16 intake posts were engine/operator-filed orders excluded).
   Caveat: the audit re-verdict (real finding — bare-quoted includes in moved
   TUs) landed ~20s AFTER the issue closed. The close beat the NEED.
2. **Zero prose-parser bugs: PASS.** No inbox tail scans, no verdict regexes,
   no CTX adjudication anywhere in the herdr path. The failures this pilot
   found were schema gaps (audit tip/ACK grammar), fixed as schema, tested.
3. **≥2 rooms concurrently, no lane theft: PARTIAL.** Room #403 onboarded
   itself (RECON demand auto-filed, ticking, zero cross-room interference).
   But lanes are labeled per-room and only room 420 ever got lanes assigned —
   403 was never staffed. Concurrency proven at engine level, not at lane
   level. The slot table + admission queue remain sim-tested only.
4. **Every wedge self-resolves or surfaces a typed NEED: PASS WITH NOTE.**
   No silent wedge occurred; every stall candidate either advanced or hit an
   explicit NEED. Note: the livelock detector keys on consecutive NEEDs; an
   alternating NEED/READY argument with evolving text never trips it. The
   pilot's longest argument resolved by pushing, not by detection.
5. **Operator additions (worker-bound tracking, output-in-boundary): DONE.**
   `rung_to` recorded on every ring; gate decisions require ACK of an open
   lane demand; validate-on-read quarantines unvalidated stream lines
   (`posts.rejected.jsonl` + `rejected=` in status). The MCP override serves
   the branch server to pilot checkouts (global config points at main).
6. **No human chats: NOT MET.** Spawn briefs + 2 substantive re-briefs +
   one complete-work instruction were human-sent. The loop ran unattended
   for long stretches (longest: full RECON→IMPL→VERIFY cycle with zero
   human messages), but the record must show the interventions.

## What the pilot proved (durable)

- Typed wire end to end: 280+ posts, every gate read from validated JSON;
  malformed and foreign-lane posts rejected with the exact fix, twice
  self-corrected by lanes without human help.
- Fenced parallelism without shared sessions: no global scans, no
  IDLE-claim storms, no cross-room pending reads — the legacy wedge class
  is structurally absent.
- The engine finds and fixes its own integration bugs when they are
  observable in-band (label lookup, tmp clobber, ACK grammar, quiet loop).

## What the pilot did NOT prove

- Lane protocol discipline under no supervision from a cold start (all
  lanes were briefed by a human at spawn).
- qe testing beyond read-only verification (no builds, no QEMU runs).
- Multi-lane, multi-room throughput (one dev, one qe, one audit; second
  room never staffed).
- The close itself was human-authorized lane action (dev closed #420 with
  the operator's credentials under the assigned workflow), and it raced a
  real audit finding.

## Recommendation

- **Adopt the substrate for new parallel-room work** (namespaces, typed
  wire, gates, ladder, scheduler), with human briefs at spawn accepted as
  the standing procedure — the "no human words ever" bar is not load-bearing;
  the load-bearing property is *no human words mid-flight*, which held for
  the full VERIFY→CLOSE-OUT walk.
- **Do not cut over the legacy engine yet**: port the MCP server pointer
  (global config → branch file is pilot-only), keep legacy rooms on tmux
  until a second pilot walks a room with zero spawn briefs.
- **Follow-ups from the pilot, in order**: (1) bare-quoted includes in the
  moved TUs (open audit NEED vs closed #420 — reopen or file a follow-up);
  (2) qe test execution on real builds; (3) second staffed room to prove
  lane-level parallelism; (4) close-out automation for the CLOSED derivation
  (currently recorded by hand from the GitHub API).

## Rollback

Unchanged from §8: stop herdr terminals, `rm -rf /tmp/nherdr-*`, keep
`engine-herdr` as history. Nothing on `main` was ever touched; the pilot
worktrees are detached and disposable; `pilot/420-colocation` is an ordinary
branch on origin.
