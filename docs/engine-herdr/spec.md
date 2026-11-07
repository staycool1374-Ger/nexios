# engine-herdr spec v1 (seed elaboration of issue #507 §5)

Status: **drafted at T1, awaiting operator review before T2** (task T1 gate:
"spec.md reviewed by operator"). Everything below elaborates #507 §5; where
T0 produced evidence that §5 glossed over, the finding is folded in and
marked `[T0]`.

Branch: `engine-herdr` · Pilot namespace: `/tmp/nherdr-<issue>/` · The legacy
engine keeps running on `/tmp/nexios-<issue>/` untouched until T9's verdict.

## 1. Runtime namespace (T1)

```
/tmp/nherdr-<issue>/
  state.json     # typed room state (the only engine-writable state)
  prompts/       # rendered role fences + order texts (files, not chat)
  reports/       # worker/audit reports (files, referenced by oid)
```

No prose inboxes (`inbox/<lane>.md`) by default: structured posts are the
input medium; inboxes, if ever wanted for auditability, are **rendered
JSON-lines views** produced by the transport (§3), never parsed back.

## 2. Sources of truth — gates read ONLY these `[T0-amended]`

1. **herdr** — terminal existence, lifecycle (`working|blocked|idle|done`),
   `events` stream, `wait` results.
2. **Structured posts** — schema-validated `post_update` JSON (§3); inboxes
   are rendered views, never parsed.
3. **Git** — `rev-parse` / branch tips / `merge-base` diffs / diff coverage.
   Any claim (`branch:`, `tip:`) that does not resolve to a ref is a typed
   rejection, not a NEED.

**2b. Screen-shaped adjacency (opencode only) `[T0]`:** opencode is a plain
program for herdr — its turn/modal state is not agent-reported. Where a gate
needs identity of a *blocking condition* on an opencode terminal, it may
match a stable on-screen header (e.g. `Permission required`) via
`hc wait --output`; matching headers is not prose parsing of worker speech.
Eliminate 2b as soon as a herdr-agent bridge or an opencode status reporter
exists (open question Q1 → operator).

`[T0]` spawn identity: every terminal is validated against the four-value
fence (PWD, OPENCODE_CONFIG, NEXIOS_AGENT_ROLE, TMPDIR) on the opencode
process env before the engine declares it owned; mismatch ⇒ `hc kill`. On
macOS only unsigned programs expose `ps eww` environ (opencode does; zsh/sh
do not) — the seam reads the agent process, exactly like legacy
`check_fenced`.

## 3. Transport (T3) — wire schema v1, evolves `post_update`, OID doctrine kept

```json
{"v":1,"oid":"qe:2026-11-07T12:00:00","from":"qe","kind":"READY|NEED|NOTE|DECISION",
 "state":"working|idle|waiting|blocked","next":"<declared next step>",
 "acks":["qe:2026-11-07T11:52:00"],
 "evidence":{"branch":"refs/heads/x","tip":"<sha>","base":"main","diff":"covered|pending"},
 "ctx_pct":65,"ts":"2026-11-07T12:00:00Z","room":474}
```

- Malformed fields ⇒ **typed rejection event** to the lane (schema error,
  field, hint). Never silent, never speech.
- OID grammar: `dev:<ts19>` / `next:<role>:<hash8>` / `peer:<role>:<ts19>` /
  `first:<role>`; an ACK clears exactly one oid.
- ACK ledger: match/prune in `state.json`; fake ACK (unknown oid) is a
  rejection; stale CTX adjudication reads timestamps only.
- Arrival trigger: `hc events` supply wake events (`[T0]` typed created/
  foreground/finished lines); no polling of file tails.

## 4. Slim FSM (T4) — the domain core that survives

- Phases: `RECON → IMPL → VERIFY → CLOSE-OUT → CLOSED` (CLOSED is
  GitHub-derived; the engine never originates closure).
- Era guards: compare **timestamps of structured posts + fetched refs**
  only — all prose gates from `docs/state-machine.md` §8 are dropped.
- Orders: `filed(oid)` with no deadlines → ring via `hc send` (max once
  per cycle) → `acked` on lane ACK (claimed/working) → `blocked` on lane
  NEED (handed back with a note; worker freed; re-ringable by a DIFFERENT
  worker) → `retired` on gate. Waiting is a valid state: unclaimed tasks
  wait indefinitely — no timeouts, no escalation, no parking, no closure.
- Auditor: instance per demand (`hc run --bg` one-shot/two-turn); verdict
  identity `kind+tip+demand`; teardown after 2 idle cycles; relaunch budget
  3 ⇒ operator NEED; re-audit re-opens the era.
- Slot table (T6): `2 dev worktrees + 2 testbed worktrees`; admission
  queues per kind (impl/test/audit); scheduling decided by the FSM.
  **Cross-room pending-text scans and IDLE-claim scans are banned.**

## 5. File map (unchanged from §5.4)

```
docs/engine-herdr/spec.md          # this file (T1)
scripts/herdr_engine/              # python3, stdlib only (T2–T7)
  herdr.py transport.py fsm.py auditor.py scheduler.py escalate.py cli.py
scripts/herdr_up.sh                # verbs: setup / testbed-rooms / status / selftest (T1+)
docs/engine-herdr/spawn-proof.md   # T0 evidence (done)
tests/herdr_engine/                # ≥300 selftest checks incl. adversarial posts
```

## 6. Handover (T7, repurposed post-durable-queue)

`nudge → re-ring → re-create (hc kill + rm of the dead terminal)` is a
MANUAL operator-invoked poke toolkit. It has no terminal rung and the
engine never fires it automatically: waiting is a valid task state, not
a violation. Task movement is mechanical, never temporal:

- WAITING (`open`): provisioned, queued, unclaimed. No deadline, no
  escalation, no expiry. Re-rung whenever a new worker appears; quiet
  (no action, no noise) while none exists.
- Claimed (`acked`): a lane ACKed the order; the ring binds the message
  to one terminal. A dead bound terminal voids the claim (event-driven
  on `hc get` failure — no counting); the task itself is untouched and
  simply waits again.
- BLOCKED (`blocked`): the holding lane posted NEED. The order returns to
  the queue carrying the handover note; the worker is freed the moment
  the claim clears and may take other tasks; the engine re-rings the
  task to a DIFFERENT terminal (the handing-back one is excluded). A
  lane may reclaim by ACKing again.
- No other terminal state exists: a task is never parked, closed, or
  timed out. `verify_identity` then kill-on-mismatch still guards every
  `[T0]` re-create.

## 7. Pilot contract (T8)

- One real open issue; legacy engine AND herdr engine run concurrently;
  herdr never touches legacy rooms (separate namespace + separate
  worktrees `~/jarvis-herdr-dev-wt1`, `~/jarvis-herdr-testbed-wt1`).
- Comparison in the room's `reports/` + one comment on #507: ticks,
  human interventions, gate decisions, per-phase wall-clock, wedges.
- Run ≥4h or until the room walks DONE→CLOSED, whichever first.

## 8. Rollback

Stop herdr workspaces + `rm -rf /tmp/nherdr-*`; `engine-herdr` stays as
history. Nothing on `main` changes at any point.

## 9. Open questions for the operator (T1 review)

- **Q1** [T0]: keep `wait --output '<stable modal header>'` as the only
  screen-shaped gate (2b), or require a bridge/reporter before T8?
- **Q2**: model quota exhaustion op — full selftest of C14/C15 needs a live
  free-tier model; pin `HC_SELFTEST_MODEL` at T8 or leave the SKIP-degrade?
- **Q3**: pilot-room candidate — a clean `testing`- or `audit`-labelled
  open issue (pick at T8 per §5.5) — any preference now?
- **Q4**: auditor spawn shape (one-shot `hc run --bg` with task vs two-turn)
  — default two-turn unless overridden at T5.

## 10. Adopted decisions (2026-11-07, operator go-ahead — §9 closed)

- **Q1** → keep the screen-shaped gate (§2b) as the only opencode-modal
  detector for the pilot; the hc-agent bridge/reporter question moves to
  the T9 verdict memo. Rationale: header-constant matching already proves
  (T0), no new integration work, degrades to investigation-need not silence.
- **Q2** → keep the SKIP-degrade for `--full` selftest gates; pin
  `HC_SELFTEST_MODEL` only in the T8 pilot lane (whichever model has live
  quota at pilot start).
- **Q3** → pilot room is **#420** ("Test code colocation", zero comments,
  no legacy `/tmp/nexios-420` room, no branch). Note: the only
  `testing`-labelled open room (#474) is disqualified — it has an active
  legacy VERIFY room, a live `testbed` branch, and unresolved follow-ups
  (violates §5.5's "never touches legacy rooms"). No open `audit`-labelled
  room exists.
- **Q4** → two-turn interactive auditor instances by default (T5 as built);
  re-audits use the idle×2 teardown path. Flip to one-shot only if the
  pilot proves churning.

## 11. Operator directive (2026-11-07, post-T9 — durable queue)

- Tasks are durable queue entries: a provisioned task waits until a
  matching worker claims it — 10 seconds or 10 days. No ack-by/done-by
  deadlines, no overdue counting, no halt budget, no terminal NEED, no
  parking, no closure. Waiting is a valid state (WAITING); a held task
  that needs a different worker is handed back (BLOCKED) and the freed
  worker may take other tasks — concurrency, not escalation.
- Rationale (operator): a timeout on an unclaimed task punishes the task
  for the scheduler having no worker; parking/closing destroys provisioned
  work. The old rule confused "nobody picked it up yet" with "wedged".
- What stays: crash-looping worker instances still flag instance-level
  `operator_needed` (broken worker infra, not task fate — the task itself
  returns to the queue); identical-NEED live-lock still flags room-level
  `stalled` (workers actively arguing, not waiting).

## 12. Owner ruling (2026-11-07 — audit attaches to authorship, not issues)

- **qe-authored deliveries: no audit step at all** (operator ruling (b)).
  Ground: the owner rule — developer ships kernel mechanisms, QE owns
  test-group authorship. A qe lane is kernel-incapable by role, so a
  qe-authored delivery cannot taint the feature; there is no audit object.
  No confinement check either: authorship is ledger-verified (`from` on
  every post), and the incapacity is structural to the role.
- **dev-authored deliveries: audit as written** (unchanged). The RECON
  scope NOTE, any kernel touch, any dev implementation — all gated
  exactly as before. #353 (kernel touched, SIL 3) vs #403 (vehicle-only)
  is the standing precedent pair.
- Consequence for #403: dev's RECON NOTE is dev-authored → one audit
  verdict to open the IMPL gate. Everything after (GDB proofs, T3 vehicle,
  verification) is qe-authored → proceeds with no audit step.
- LLMs never amend this: any delivery whose authorship is ambiguous, or
  any dev-authored diff touching production paths, escalates to the
  operator instead of routing around the gate.
