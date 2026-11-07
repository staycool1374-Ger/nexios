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
- Orders: `filed(oid, ack-by +900s, done-by +1800s)` → ring via `hc send`
  (max once per cycle) → escalation ladder (§6) whose last element is a
  typed operator NEED.
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

## 6. Escalation (T7)

`nudge → re-ring → recreate (hc kill + fenced respawn) → typed operator
NEED (owner + reason + evidence)`. No path ends in silence; halt budget
carries over. `[T0]` recreate re-verified by the §3 identity seam
contract — `verify_identity` then kill-on-mismatch are already test-proven.

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
