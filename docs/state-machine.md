# Nexios engine: state machine + environment

Operational documentation for the deterministic multi-issue orchestration
engine that drives the dev / qe / auditor lanes. This is **tooling**, not a
kernel spec: it binds the operator-facing contracts of `scripts/agent_up.sh`,
`scripts/state_machine.py`, and their runtime environment.

Source of truth is the code. This document quotes it; where they disagree,
the code wins.

- Engine: `scripts/state_machine.py` (`VERSION = "state-machine v3 …"`)
- Launcher: `scripts/agent_up.sh` (canonical location: the repo checkout —
  a stale copy under `~/.local/state/nexios/bin/` is superseded, delete it,
  never run it)
- Companion: `scripts/watchdog.sh`, `scripts/gate.py`,
  `scripts/deadlock_watch.py`, `scripts/healthcheck.sh`
- Lifecycle picture: `state-machine-lifecycle.png` (room phases, triggers, actions, resulting states)
  + `state-machine-lifecycle.svg` (editable vector source)

---

## 1. What the engine is

The engine **replaces the former coordinator role outright**. The module
docstring states the doctrine: *the engine is the code-driven coordinator;
the consultant is a publishing advisor, never an orchestrator*. All
orchestration, worker execution orders, workflow transitions, the auditor
lifecycle, and modal mitigation run in one deterministic program, driven by
watchdog ticks.

Deterministic means: identical room inputs always produce identical
decisions. Every decision is logged (`logs/state-machine/`) and every
incoming worker post is ACKed with a recorded transition. The engine is a
single headless heartbeat — one tmux session (`nexios-engine`), one sweep
lock, round-robin room order — never concurrent orchestrators.

The consultant session (`nexios-consultant`) is the operator's advisor
window. It never drives workers, rings sessions, or touches state;
orchestration belongs to the engine.

## 2. Environment

### 2.1 Machine layout

| Path | Role |
|---|---|
| `~/jarvis` (`DEV_DIR`) | repo checkout; the dev lane works here |
| `~/jarvis_test` (`QE_DIR`) | testbed checkout; the QE lane works here |
| `$NEXIOS_HOME` (`~/.local/state/nexios`) | rendered fences (`rendered/<role>/opencode.resolved.json`), consultant home; links only — a real file out of repo is versioning drift and hard-fails |
| `/tmp/nexios-<issue>/` | room runtime: inbox, `state.json`, prompts, notes, `PROTOCOL.md` (per-issue, outside the repo) |
| `<repo>/logs/state-machine/` | the engine's only repo writes: `state-machine.log`, `engine-console.log`, `engine.lock`, `state-graph.json`, `STOP` sentinel |
| `/tmp/agentup-tmp-<session>/` | pinned `TMPDIR` namespace, one per fenced session |

### 2.2 Prerequisites

| Requirement | Why | Checked by |
|---|---|---|
| `tmux` | all sessions are tmux sessions; fencing needs process env inheritance | `need_cmd tmux` before every launch |
| `gh` (authenticated) | label reads, closure poll, transition notes, decision relay | `gh_bin()`; fail-loud reads, fail-open refresh |
| `python3` (+ `fcntl`) | the engine; all state writes are `flock`-guarded | `need_cmd python3` |
| `git` | branch/worktree inspection, refs-only origin fetch | used throughout; never auto-updates a worktree |
| opencode binary (`$OPENCODE_BIN`, default `~/.opencode/bin/opencode`) | worker runtime inside sessions | `need_cmd "$OPENCODE_BIN"` before every launch |
| fence sources `tools/agents/fence/` + `tools/agents/render_fence.py` | rendered per-role configs | `need_file` before render/launch |
| pinned `TMPDIR` | the host `TMPDIR` (`/var/folders/…` on macOS) is per-boot and cannot be fenced; an unfenced `TMPDIR` is how writers fell back to unfenced bash during the #410 burn-in | launcher pins `TMPDIR=/tmp/agentup-tmp-<session>` |

Why tmux and not `harness new`: fenced sessions need `OPENCODE_CONFIG`
in their process env. `harness new` talks to the daemon over IPC, which
drops the invoker's env, so harness-managed sessions launch **unfenced**
(proven: S1 step-zero fails there). Direct tmux launch inherits the env,
so fences hold (proven: S1 passes). Tradeoff: tmux sessions are invisible
to `harness ps`. Watch via `tmux attach -t <name>`.

### 2.3 Sessions

| Session | Content | Ownership |
|---|---|---|
| `nexios-engine` | headless loop: `python3 scripts/state_machine.py tick --all --interval 60 --allow-recreate` | engine only; `wake … nexios-engine` skips the keystroke (append-only) because keystrokes would corrupt a running loop's input |
| `nexios-dev` / `nexios-qe` | worker TUIs, launched with `prompts/dev.txt` / `prompts/qe.txt` from the room | engine rings; operator watches |
| `nexios-audit` | auditor, headless: launched from `<runtime>/prompts/auditor.txt` (`--agent nexios-audit --auto`, ask auto-approved, deny enforced) | engine launches, hands off verdict, tears down after 2 idle ticks |
| `nexios-consultant` | advisor TUI (repo write except `src/`, full `/tmp`, GitHub issues/PRs/releases) | operator; never orchestrates |

Env overrides exist where it matters (`NEXIOS_DEV_SESS`, `NEXIOS_QE_SESS`,
`NEXIOS_AUDIT_SESS`, `NEXIOS_DOORBELL_SESS`, `NEXIOS_AGENT_UP` in
`watchdog.sh`).

### 2.4 Room runtime (`/tmp/nexios-<issue>/`)

Created idempotently by `setup`: never overwrites existing state/inbox.
`state --reconstruct` rebuilds machine fields into a `--copy` only — the
live `state.json` is never touched.

| Entry | Content |
|---|---|
| `inbox/dev.md`, `inbox/qe.md`, `inbox/auditor.md` | lane inboxes, shared append-only; receivers read at natural step boundaries |
| `inbox/coord.md` | ordered durable WAKE lines (`<ts> FROM=<role> WAKE :: <text>`) |
| `state.json` + `state.lock` | single-writer JSON state (section 5); `flock` serializes engine vs `state` mode |
| `prompts/dev.txt`, `prompts/qe.txt`, `prompts/auditor.txt` | RECON briefs / role prompts the engine writes, workers execute |
| `notes/` | scratch the lanes may use |
| `PROTOCOL.md`, `ANCHOR.md` | comms rules (pointer-only rings) and session anchor |

### 2.5 GitHub surface

Repo `staycool1374-Ger/nexios`. The engine reads issue labels
(`audit`, `testing` → room scope, section 5.3), polls closure (at most every 5
ticks in merge-close), posts opt-in transition notes (`gh_notes`, section 4),
and relays operator decisions verbatim (section 8.5). It never originates CLOSED:
terminal state is derived from the GitHub closed flag.

## 3. Scripts reference

### 3.1 `agent_up.sh` modes

| Mode | Effect |
|---|---|
| `setup <issue>` | create room runtime + render every role fence; idempotent |
| `consult [taskfile]` | ensure fenced advisor session (no attach; engine-safe) |
| `windows` | open visible Terminal windows for consultant + dev + qe (read-write mirrors only — read-only `-r` mirrors block ALL tmux input including doorbells, so none here) |
| `workers [issue]` | launch dev + qe sessions (detached); with an issue they get room prompts, without they start bare and wait for a ring |
| `up [issue-or-list] ["task"]` | full system from one zsh command: per-issue setup, RECON briefs from issue text (missing only), one shared workers launch, consultant, headless engine loop, three Terminal windows; disarms legacy per-room tickers; parked lanes resume from declared NEXT (ring ledger caps re-rings). Bare `up` resumes sessions in place — cold starts go through provision |
| `run <name> <dir> <role> <prompt> [branch]` | launch ONE session; the fence is rendered from repo source at launch, never from a path |
| `audit <issue>` | launch the headless auditor from the room prompt |
| `audit-diff [--base]` | write the audit diff to `audits/pending_patch.diff` (non-empty base required; `git add -N` brand-new files first) |
| `qe-merge <issue>` | merge `origin/main` into the testbed `--no-commit --no-ff`; abort-on-conflict restores code state |
| `msg <sess> <text>` | single-line doorbell (either direction) |
| `wake <issue> <from> <sess> <text>` | doorbell + durable WAKE line; fails loud when runtime/inbox is absent; engine target skips the keystroke |
| `comms-test [issue]` | prove the doorbell both ways with scratch echo sessions (no tokens, production sessions untouched) |
| `state <issue> k=v…` | locked `state.json` write, validated keys/values only |
| `status [issue]` | tmux + branches + inbox + state overview, read-only |
| `status --reconstruct` | rebuild into a `--copy` (mandatory), never live |
| `fence show <role>` / `fence install` | print the composed permission map / regenerate rendered configs + subagent files |
| `render-self-test` | fence renderer/verifier matrix (no tmux, no opencode) |
| `self-test` | guard + helper self-tests; scratch tmux names only, never production sessions |
| `approve <sess>` | drive one Allow-always on the session's current permission dialog — person-run only |
| `close [--all]` | kill worker sessions; `--all` also kills consultant + engine loop |
| `clean <issue> [--dry-run]` | remove a CLOSED lane's merged branches + room; **refuses on open issue, unpushed main, or live room** |
| `watch <issue> [arm\|disarm\|tick\|status]` | dev/qe activity watchdog; legacy — the engine loop owns ticks, tickers stay disarmed |

### 3.2 `state_machine.py` commands

| Command | Effect |
|---|---|
| `tick [--issues N,… \| --all] [--interval S] [--once] [--dry-run] [--log-dir DIR] [--repo DIR] [--no-audit-teardown]` | the sweep: per-room `tick_room`, shared ring/busy sets, sweep lock, round-robin order, quiet/all-done/STOP exit. `--once` = single cycle (`interval 0`). `--dry-run` logs `WOULD …` and takes no lock (dry runs never write state) |
| `status [--issues …]` | read-only serial table, no mutation |
| `brief --issues … [--dev-branch]` | RECON briefs only |
| `provision` | per-issue setup + RECON briefs from GitHub issue text |
| `selftest` | pure-function suite (697 checks): gates, phases, transitions, scopes, briefs |

### 3.3 Companions

| Script | Role |
|---|---|
| `watchdog.sh` | in-repo dev/qe activity watchdog (`tick\|arm\|disarm\|status`); runtime files live ONLY in `/tmp/nexios-<issue>/` (`watch.pid`, `watch.unchanged`, `watch.log`), never in the repo; POSIX-ish bash, no new deps. Protocol touchpoint: reads rooms/inboxes, emits WAKE via `wake` only when armed (legacy — tickers stay disarmed, the engine loop owns ticks); never writes `state.json`, lane inboxes, or GitHub |
| `gate.py` | mechanical AGENTS.md workflow gate, separate from the engine: `check-close` (close needs a `DECISION: APPROVED` comment), `check-commit` (message references an open issue), `check-feedback`. Reads issue comments; never writes inbox, state, or orders |
| `deadlock_watch.py` | read-only diagnosis (`MOVING / PARKED-OK / DEADLOCK` + why-chain from room state + log tail); no writes, no ticks, no doorbells — not even WAKE |
| `healthcheck.sh` | RTOS toolchain pre-flight (compilers, qemu, grub, renode, workspace); no protocol touchpoint — never reads/writes rooms, inbox, state, or GitHub |
| `run_all_classes.sh`, `run_config_matrix.sh` | kernel-side build/test tooling, outside the engine |

## 4. Fences and trust boundaries

1. **Rendered fences.** Every role fence is rendered from `tools/agents/fence/`
   sources at launch into `$NEXIOS_HOME/rendered/<role>/opencode.resolved.json`,
   in one code path so a role can never be written to another role's file.
   A caller passes a role *name*, never a config path.
2. **Pre-prompt verification (S4-9).** Before any prompt is delivered, the
   session env must show the expected `PWD + OPENCODE_CONFIG +
   NEXIOS_AGENT_ROLE + TMPDIR` match set; a session that fails the match is
   **killed rather than trusted**.
3. **Lane/checkout mapping.** Dev works the repo checkout, QE the testbed
   checkout — and the *engine itself* never writes kernel code, test files,
   worker checkouts, or fence sources. It writes repo-bound logs, `/tmp`
   room briefs, `state.json`, doorbells, and auditor launches. Nothing else.
4. **Clean-tree guard.** `git add -A` must never run blindly while tooling is
   untracked; the canonical `agent_up.sh` is the repo copy.
5. **`clean` guards.** Refuses on open issue, unpushed main, or live room.
   Branch deletion is gated explicit delete (never `-d`/`-D`); only merged
   branches are deletable, anything else reports KEPT with a reason.
6. **Fetch policy.** The engine refreshes `origin` refs (every 5 ticks,
   fail-open: on failure it continues on local refs) and warns on stale
   checkouts — but **never auto-updates a worktree**. Moves stay explicit
   engine orders (`qe-merge`, provisioned branches).
7. **GitHub write fence.** Transition notes are opt-in per room
   (`state <issue> gh_notes=true`, default off: a robot posting as the
   operator must be explicitly armed), one comment per event, deduped via
   `state.json: gh_posted`. Decision briefs are the exception: a lane NOTE
   carrying an explicit `REQUEST:` line is relayed as-is (chunked past the
   API cap), attributed with lane + timestamp. The lane's explicit request
   is the arm; no robot-authored content posts.
8. **Lock discipline.** `state.lock` per room (only sanctioned writers: the
   engine and `state` mode, atomic mkstemp + fsync + replace). `engine.lock`
   per sweep, `LOCK_EX|LOCK_NB`: a fast-path one-shot tick that finds the
   lock held logs `cycle skipped: another engine tick holds the lock` and is
   a no-op — the lock is never held across `sleep(interval)`.
9. **Stop discipline.** `SIGINT/SIGTERM/SIGHUP` and the `STOP` sentinel file
   both stash state and exit cleanly with a summary; the sentinel is consumed
   (unlinked) on exit.
10. **Ring doctrine.** An order is rung at most 3 times (`RING_LIMIT`);
    then NEED-recreate, never a 4th ring. Worker orders carry
    `ack-by <filed+900s>` and `done-by <filed+1800s>`; overdue ACKs get one
    nudge, overdue DONEs one re-ring, then escalation (`ACK-MISSING`,
    NEED escalation after a 900 s grace). The ring ledger caps re-rings and
    resumes parked lanes from declared NEXT.
11. **Retry budget → halt.** `retries / retry_budget` (default 3); exhausted
    means `halt_now`: the room reports `HALTED: no actions` and the engine
    performs no further actions on it. Explicit `halt=true` does the same.

## 5. Internal state

### 5.1 `state.json`

Operator-writable keys (`state` mode allowlist): `audit_branch`, `halt`,
`pending`, `phase`, `retries`, `retry_budget`, `state`, `updated`,
`release_armed`, `gh_notes`, `scope`. Engine-managed ledger keys observed
in the code: `rings` (re-ring ledger), `handled` (per-lane ACK watermarks),
`gh_posted` (comment dedup), `delivery` (ack/done SLA tracking),
`ack_failures`, `gh_closed`, `qh_poll`, `scope` (backfilled once).

Room-state vocabulary (`state` mode enforces it): `ISSUE_OPEN`, `PLANNED`,
`IN_PROGRESS`, `AUDITED`, `VERIFIED`, `CLOSED`, `FEEDBACK`, `CLOSING`.

### 5.2 Derived phase (never stored blindly)

`derive_room_phase` computes `RECON | IMPL | VERIFY | CLOSE-OUT | CLOSED`
from lane evidence timestamps each tick — era-guarded:

- `CLOSED` only when the issue is closed (never originated by the engine).
- `CLOSE-OUT` only when the work is demonstrably finished **and** nothing
  newer re-opened the era: the approval must not postdate the DONE, and no
  gated IMPL READY may postdate the DONE. A newer plan approval or a newer
  IMPL READY re-opens the era (`VERIFY` while a gated READY stands, `IMPL`
  once the plan is approved).
- `VERIFY` on any IMPL READY or QE VERIFIED; `IMPL` on any approval;
  otherwise `RECON`. A QE VERIFIED proves the era but never blocks
  CLOSE-OUT (the closeout stage owns merge-close).

`next_room_state` layers the stored state on top: `CLOSE-OUT +
IN_PROGRESS → CLOSING`; any other phase with `CLOSING` regresses to
`IN_PROGRESS` (era re-opened) unless GitHub already closed; `CLOSED` is
terminal and never regresses.

### 5.3 Room scope (lane labels)

`scope_for_labels` maps GitHub labels to `full | audit-only | testing-only`
(`testing` without `audit` → `testing-only`, `audit` without `testing` →
`audit-only`, else `full`; case-insensitive). Seeded at `setup` from one
`gh` call (fail-open → `full`), backfilled once by the engine for older
rooms, stored in `state.json`, honored by briefs (dev stays out of scoped
rooms except merge-close; QE owns testing with a tip + diff + branch
evidence contract), and enforced by demand gating (IMPL promotion/stalled/
rebrief and backlog candidacy are full-scope only) and closeout branching.

## 6. Message protocol (inbox wire format)

Everything the lanes and the engine say to each other travels through
`/tmp/nexios-<issue>/inbox/` as single-line atomic appends. There is no
other channel: no chat payloads, no direct session writes, no shared
memory. Direction matters — each side writes only its own files, and the
doorbell (tmux keystroke) carries no payload, only a pointer.

| Direction | Writer | File | Reader |
|---|---|---|---|
| lane → engine | worker session | `inbox/<role>.md` (own box: dev|qe|auditor) | engine tick (ACK-scan) |
| lane → engine (durable) | `wake` command | `inbox/coord.md` (WAKE lines, ordered) | engine tick (wake scan, resurface) |
| engine → lane | engine tick | `inbox/<role>.md` (order notes) | worker at step boundaries |
| either way | `msg` / `wake` | tmux keystroke only — pointer, never payload | session re-reads its inbox |

### 6.1 Lane posts (lane → engine)

Grammar (`POST_RE` — one post per line):

```
<ts> FROM=<dev|qe|audit|auditor> <READY|NEED|NOTE|DECISION> :: <text>
```

Example:

```
2026-11-05T07:34:58Z FROM=qe READY :: TESTING complete, tip abc1234
STATE: verifying
NEXT: idle — waiting merge
ACK: qe:2026-11-05T07:10:11
```

| Field | Meaning |
|---|---|
| `<ts>` | UTC `YYYY-MM-DDTHH:MM:SSZ` — the lane's only clock; every ledger id, cursor, and SLA derives from post timestamps, never wall-clock reads |
| `FROM=` | lane identity; must match the box file the line sits in |
| `READY | NEED | NOTE | DECISION` | `READY`: deliverable with evidence. `NEED`: blocked, names what is missing. `NOTE`: discussion, progress, or a `REQUEST:` operator question. `DECISION`: lane-side decision claim |
| `:: <text>` | first line is the verdict; details follow on later lines |

Trailers (optional lines after the post, same append):

| Trailer | Effect |
|---|---|
| `STATE: <…>` | lane-declared coarse state (informational; the engine derives its own phase) |
| `NEXT: <…>` | declared next step — the engine parks on it (`awaits:`, resume rings, wait-target parsing all read NEXT). An idle/empty NEXT on a WORKING lane reads STANDING BY in status |
| `ACK: <oid>` | clears exactly that engine order id from the ring ledger (explicit acknowledgment) |
| `CTX: <pct>%` | context pressure; 70 % watch, 85 % checkpoint-then-`/compact` — never die at 100 % with unposted work |

### 6.2 ACK ids and matching

Order-id grammar (`ACK_OID_RE`, byte-identical with the MCP worker-state
helper), with `<ts19>` = `YYYY-MM-DDTHH:MM:SS`:

- `dev:2026-11-05T07:34:58` / `qe:2026-11-05T07:34:58` — lane order filed at ts
- `next:qe:a1b2c3d4` — resume/next-step order for the lane
- `peer:dev:2026-11-05T07:34:58` — cross-lane handoff order
- `first:qe` — first-contact order to a virgin lane

`match_ack` classifies every ACK trailer: `ack` (ref equals the live
`coord_last` order, a ledger ring, or a resume order — clear exactly it),
`ignore-malformed` (bad shape or wrong lane), `ignore-stale` (well-formed
but not outstanding). ACK scanning is cursor-based per lane (`handled`):
fresh posts normally, but a virgin lane's full history on first sight, so
a first post carrying the order's ACK is actionable. A WAKE resurfaces the
last post at-or-before its timestamp exactly once (G1 one-shot re-drive),
so a WAKE whose post sits at/below the cursor is re-evaluated, never
silently dropped.

### 6.3 WAKE lines and doorbells (durable lane → engine)

Grammar (`WAKE_RE`):

```
<ts>Z FROM=<dev|qe|audit|auditor|coord> WAKE :: <text>
```

`wake <issue> <from> <sess> <text>` appends the line to `inbox/coord.md`
*and* sends one keystroke; `msg <sess> <text>` sends only the keystroke.
Target `nexios-engine` skips the keystroke (append-only — the loop is not a
TUI). Rules, from the launcher header: single line, inbox POINTER only,
idle sessions only, never touch permission dialogs, never execute ring
text. A ring means "re-read the inbox".

The watchdog's armed ticker speaks this same bus (`wake … coord … "inbox:
TICK — …"`, `watchdog.sh:199`) — the one sanctioned non-lane WAKE
producer, and only when armed (legacy: tickers stay disarmed).

### 6.4 Engine orders (engine → lane)

The engine appends order notes to the lane box (atomic single-line
`append_box_note`) and rings the session. Orders carry the ledger id plus
`ack-by <filed+900s>` and `done-by <filed+1800s>`; the lane answers with an
`ACK: <oid>` trailer on its next post. The operator decision relay returns
through this same path: the operator's `DECISION:` line is written into
the requesting lane's inbox verbatim (attributed, timestamp-deduped) and
rung — never paraphrased, never a second channel.

## 7. Status: reading the fleet

Two read-only surfaces, no mutation, no rings, no filings.

### 7.1 `agent_up.sh status [issue]`

Machine truth, four blocks: `tmux ls`; current branches of the dev and QE
checkouts; per-box inbox tails (`inbox/dev|qe|auditor|coord.md`, last 2
lines each); and the `state.json` operator fields
(`phase,state,retries,retry_budget,halt,pending`). `status --reconstruct
<issue> --copy <dir>` rebuilds machine fields from git + issue comments
into the copy — the live `state.json` is never touched.

### 7.2 Engine `status` table

`state_machine.py status` prints `next tick …` (from the state-graph
heartbeat; `OVERDUE by Ns` when the loop missed its interval), `engine
started … (uptime …)` (from `engine-restart.log`), then one table per
room headed `#<issue> state=<issue-state>` with columns
`WORKER | STATE | ACTIVITY | PLANNED`.

**Header `state=`** (`issue_state_for` — strictly phase-driven, never
persisted): `open` (RECON, auditor silent) · `planned` (RECON, auditor
posted) · `implementing` (IMPL) · `testing` (VERIFY, or closing while
waiting QE/tests) · `closing` (CLOSE-OUT/CLOSING/CLOSED or gh-closed).

**STATE column** (`status_lane_state` — 1:1 WORKING relation):

| Value | Meaning |
|---|---|
| `ABSENT` | no such tmux session (passes through; auditor row reads `-` elsewhere) |
| `IDLE` | session idles and nothing claims the lane — truly free |
| `WORKING (#n)` | the focused issue only: unacked order first, then fresh post, then active NEXT (lowest issue wins) |
| `WORKING` | engaged here but the focus is elsewhere |
| `BLOCKED` | parked wait (`waits`), open operator question (`operator_hold`), or a permission modal (`BLOCKED-modal`) — a parked wait is BLOCKED, not IDLE |

**ACTIVITY column** (closed vocabulary):

| Value | Meaning |
|---|---|
| `IMPLEMENTATION` / `IMPLEMENTING TESTS` / `AUDITING` | dev / qe / auditor WORKING verbs |
| `STANDING BY` | WORKING lane whose own NEXT is explicitly idle — holds an assignment but parked |
| `WAITING <target>` (QE|AUDIT|DEV) | dev BLOCKED; target parsed from the room pending dev segment (`lane_wait_target`; non-dev lanes always wait on DEV; VERIFY/CLOSE-OUT/CLOSING default QE, else AUDIT) |
| `WAITING DEV` | qe/audit BLOCKED |
| `WAITING FOR OPERATOR` | operator question open — the operator-side next step reads `READING ISSUE #n` |
| `-` | ABSENT (or unknown) |

**PLANNED column** (`planned_enum` over the raw next-step read):

| Value | Meaning |
|---|---|
| `EXPECTING RESULT/OK` | an order, verdict, or artifact is awaited (`ring:`, `NEED`, `order filed/deferred`, `collect verdict`, `verdict on file`, `awaiting verdict`, `queued`, `ring audit`) |
| `CHECKING STATUS` | routine sweep, nothing outstanding (`no briefed work`, `no audit requested`, anything else) |
| `HALTED` | room halted — no actions |
| `-` | lane absent |

The raw read underneath is `ring: unacked order` · `ring: resume per
NEXT` · `awaits: <NEXT…>` · `pending: <…>` · `NEED idle: no briefed work`.
An idle claim is verified against the same work-conserving scan the tick
uses — if work exists elsewhere the cell reads `IDLE-denied (<kind> #n;
scan: <why>)` instead of granting IDLE. Below the tables,
`watchdog_invariant` prints one line only on violation: starved lanes
while the engine heartbeat is gone (per-room tickers stay disarmed by
doctrine, so that combination must never happen).

## 8. Lifecycle chapter: state, triggers, actions

The picture in `state-machine-lifecycle.png` shows the room-phase rail with every trigger edge. This chapter is the same
information in text, plus the lane-order lifecycles the picture summarizes.

### 8.1 Room-phase transitions (trigger → action → new state)

| # | Trigger (signal the engine reads) | Engine action | Resulting state |
|---|---|---|---|
| T1 | Approved plan + dev READY evidence → gated IMPL READY (`gate_impl_ready`) | File IMPL order to dev, ring doorbell | `RECON → IMPL` |
| T2 | Dev parked/approved but idle (`impl_promotion_due`, full scope only) | Re-file / promote the IMPL order | stays `IMPL`, dev re-engaged |
| T3 | Dev files DONE with a tip | Verify demand; file TESTING order to QE (approach handshake) | `IMPL → VERIFY` |
| T4 | QE files VERIFIED **with evidence** (tip + diff-present + branch) | Record era proof; request auditor verdict on the tip | stays `VERIFY`, auditor engaged |
| T5 | Auditor APPROVED, approval ≤ DONE, no newer READY, QE VERIFIED present | `closeout_stage` → merge-close: merge the lane branch | `VERIFY → CLOSE-OUT` → stored `CLOSING` |
| T6 | GitHub closed flag observed (poll, ≤ every 5 ticks in merge-close) | Derive `CLOSED`; retire room when all-done | `CLOSING → CLOSED` (terminal; never regresses, never originated) |
| T7 | CHANGES requested / evidence missing | `FEEDBACK` demand back to the owning lane | `VERIFY/IMPL → FEEDBACK → RECON/IMPL` on re-brief |
| T8 | Newer plan approval postdates DONE, or newer gated READY postdates DONE | Era re-opened; regress stored state | `CLOSING → IN_PROGRESS`, phase back to `IMPL`/`VERIFY` |
| T9 | `retries ≥ retry_budget` (default 3/3), or operator `halt=true` | `halt_now`: no further actions | `HALTED` (any phase; explicit, not a phase regression) |
| T10 | Operator `DECISION:` line on the issue | Relay verbatim to the requesting lane; resume parked order | lane unblocked, phase unchanged |
| T11 | Testing-only scope + QE VERIFIED tip with no verdict coverage | Synthesize audit demand from the QE tip (auditor lifecycle reused unchanged) | `VERIFY` proceeds to T5 without a dev DONE |
| T12 | `clean` guards pass on a CLOSED room (issue closed, main pushed, room not live) | Delete merged branches, remove room runtime | `CLOSED → retired` (terminal housekeeping) |

Merge-readiness additionally requires QE evidence on file
(`merge_ready_ok` over decision + VERIFIED + closeout + evidence); a QE
VERIFIED without tip/diff/branch never advances T5. QE negation
(`NOT VERIFIED`) is not a verdict — verification timestamps skip it.

### 8.2 Lane-order lifecycle (every order, every lane)

`filed (oid, ack-by +900s, done-by +1800s) → RING 1 → (no ACK) RING 2 →
(no ACK) RING 3 → NEED-recreate, never a 4th ring`. A matching `ACK: <oid>`
post clears exactly that oid. One overdue ACK draws one nudge; one overdue
DONE draws one re-ring, then escalation after the grace window
(`ACK-MISSING` / NEED escalation). The sweep rings each shared session at
most once per cycle (G2 one-ring-per-cycle, intra-sweep rung set) and rooms
are visited round-robin so the lowest issue number cannot starve the rest;
unmet demand across the sweep surfaces as NEED cycle starvation (quietly
suppressed from the operator unless it persists).

### 8.3 Auditor lifecycle (per demand)

Demand `IMPL audit <tip>` / re-audit / `audit <tip>` → auditor brief pinned
to the tip → headless launch (`nexios-audit --auto`) → verdict
(`APPROVED` / `CHANGES…`, identity `C4 = kind + tip`, handed off once) →
teardown after 2 idle ticks. Silence for 1800 s → NEED stuck; outstanding
demand re-rings once at 900 s, escalates at 1800 s; no verdict progress for
540 s → kill + relaunch, at most 3 relaunches before operator escalation.

### 8.4 QE verify contract

A TESTING order demands an approach line first (handshake), then either
`VERIFIED` with tip + diff-present + branch, or `NOT VERIFIED` (no-op for
timestamps). Baseline staleness and testbed-behind-main are re-checked on
every order (`qe-merge` restages the testbed, abort-on-conflict restores
code state). In testing-only rooms the QE tip additionally feeds the
synthesized audit demand (T11).

### 8.5 Decision relay (operator in the loop)

A lane NOTE with an explicit `REQUEST:` line is posted to the issue with a
reply hint (`DECISION:` + choice, e.g. `Y`); anything the operator writes
that does not match the `DECISION:` grammar is discussion, and an
unparseable decision-shaped line draws a clarify NEED rather than a guess.
A matching line is relayed verbatim to the lane with lane + timestamp
attribution, deduped by timestamp. The loop polls for it on wake-shaped
traffic plus a targeted fast-path tick.

### 8.6 Loop control (when the heartbeat stops)

Three quiet cycles (no progress) end the loop cleanly; all rooms all-done
emits the summary and breaks; `STOP` file or signal stashes and exits.
Every exit path emits the serial status table + state-graph snapshot, so a
restart resumes from declared NEXT with the ring ledger intact.

## 9. Tick pipeline (one sweep, in order)

1. Discover rooms (`/tmp/nexios-*`, debris skipped), rotate round-robin by
   tick sequence. Origin refs refresh (every 5th tick, fail-open).
2. Take the sweep lock (`engine.lock`, non-blocking; skip-if-held is the
   fast-path safety valve). Dry runs take no lock.
3. Per room: load boxes + handled watermarks → ACK-scan new posts →
   modal/probe sessions (fast probe pair + CPU, slow probe on demand) →
   derive evidence (DONE/READY/VERIFIED/APPROVED tips, QE evidence, gated
   READY, era checks) → lane facts, starvation scan, backlog/global scans →
   file due orders (IMPL promotion/stalled, TESTING, audit demand,
   rebriefs) → rings (once per session per cycle) → closeout/merge-close
   evaluation → GH closure poll (≤ every 5 ticks) → retry/halt accounting →
   decision-request scan → persist state (locked) → verdict table row.
4. Shared rung/busy sets propagate intra-sweep; write the state graph;
   progress accounting (quiet counter, starvation note, all-done check).

## 10. Operations cookbook

| Need | Command |
|---|---|
| Cold start one issue | `up <issue>` (setup → RECON briefs → workers → consultant → engine → windows) |
| Resume everything | bare `up` (no runtime created, no briefs written) |
| Add briefs only | `provision` / `brief --issues …` |
| Read the fleet | `status [issue]` (serial table: state, activity, planned, verdict) |
| Diagnose a stuck room | `deadlock_watch.py` (read-only) → `watch <issue> status` → log tail |
| Park a room | `state <issue> halt=true` (engine reports HALTED, takes no action) |
| Ship a room | merge-close via engine → issue closes → `clean <issue>` (guards enforced; `--dry-run` first) |
| Prove comms | `comms-test` (scratch sessions, production untouched) |
| Prove the engine | `state_machine.py selftest` + `agent_up.sh self-test` (hermetic doubles, scratch tmux names) |
| Stop everything | `close --all`; emergency: `STOP` file in `logs/state-machine/` |

## 11. Verification

- `state_machine.py selftest`: 697/697 (gates, phases, transitions, scopes, briefs, negation, dedup).
- `agent_up.sh self-test`: 201/201 (fences, guards, helpers, scope mapping, hermetic setup seeding).
- Fabricated-room dry ticks: testing-only rooms file TESTING with no dev
  IMPL; full-scope controls still file IMPL; VERIFIED + APPROVED engages
  merge-close with the branch resolved from the QE post.
- Watch discipline for risky windows (e.g. `docs/engine-wake-watch.md`):
  observation-only tallies, duplicates open follow-up issues — never inline
  patches.
