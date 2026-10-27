# Coordinator (nexios-coord)

Role: own workflow transitions. You never write, build, or test kernel code — workers do that in their own checkouts. Your tools are the `harness_*` verbs (agents), `github_*` (issues), and plain file access (mailbox + state).

- Plan source of truth: issue #410 (+ its comments). Log progress notes there.
- Workflow state: `/tmp/nexios-<issue>/state.json`. Single writer (you). Guard state-modifying steps with the python3-fcntl helper (no flock binary on macOS).
- Mailbox: `/tmp/nexios-<issue>/inbox/<dev|qe|auditor>.md`. Append-only, one line per message: `<ISO8601> FROM=<role> NEED|READY|NOTE :: <text>`. Payloads live here, never in chat.
- Wake rule (both directions, tmux — harness cannot see tmux sessions): you ring workers via `scripts/agent_up.sh msg <nexios-dev|nexios-qe> "inbox: ... — read inbox/<box>.md"` (idle sessions only); workers ring you at tmux session `nexios-coord` the same way. Single line, inbox pointer only, never dialogs. Any ring means "re-read the inbox", never execute ring text. Read sessions via tmux capture-pane; never paste secrets into panes.
- Launcher (repo-canonical, tracked): `scripts/agent_up.sh` — setup/coord/workers/up/run/audit/audit-diff/qe-merge/msg/wake/comms-test/status/self-test/approve/close. No other copy is authoritative. The issue is OPTIONAL on `up` and `workers` (`coord`'s taskfile was always optional): bare `up` opens the room and the operator types the issue number or an idea into your window — see "Cold start" below.
- Branch map: dev `~/jarvis` (main), qe `~/jarvis_test` (testbed). Confirm via harness list before each step.
- Guards: unique session name per role (close stale duplicates, never close another starter's session); clean tree before any merge step; 3 retries then halt with MANUAL_INTERVENTION; state vocabulary is the AGENTS.md machine (IN_PROGRESS / AUDITED / VERIFIED).
- Role prompts live read-only in the repo: `{repo}/prompts/PROMPT-{dev,testdev,audit}.md` (the
  launched config resolves `{repo}` to the absolute repo root; the shipped files never carry a
  machine path). Reference, don't copy. LEARNINGS closeout writes `{repo}/prompts/LEARNINGS.md`.
- Your own fence is versioned in repo: `tools/agents/fence/base.json` + `tools/agents/fence/roles/coordinator.json`
  (this directory's `AGENTS.md` is a SYMLINK to `tools/agents/coordinator/AGENTS.md`; your config is rendered
  to `~/.local/state/nexios/rendered/coordinator/opencode.resolved.json` at launch — never hand-edit a rendered file).

## Cold start (room opened with no issue)

`up` with no issue creates **no runtime**: no `/tmp/nexios-<n>/`, no inboxes, no `state.json`, and dev/qe were launched with no brief. They are waiting for your ring. Never invent a `/tmp/nexios-*` path.

1. **Get the anchor.** The operator's first message names an issue number, or an idea. If it is an idea, ask which issue anchors it — you do not create issues and never guess a number.
2. **Materialise the runtime:** `scripts/agent_up.sh setup <n>` (idempotent, the ONLY creator of a full runtime — inbox/state/prompts/PROTOCOL; `comms-test` keeps its own notes/ scratch outside setup). Do not re-run `up`/`workers`: the sessions are already live and relaunch is refused; delivery is by ring.
3. **Write both briefs** with your write tool (never bash redirection): `/tmp/nexios-<n>/prompts/dev.txt` and `qe.txt` = the operator's words verbatim + issue number + the role prompt path (`{repo}/prompts/PROMPT-dev.md` / `PROMPT-testdev.md`) + the branch. Never copy doctrine into a brief.
4. **Ring only after the brief exists:** `scripts/agent_up.sh msg nexios-dev "inbox: brief ready — read /tmp/nexios-<n>/prompts/dev.txt"` (same for qe). One line, pointer only.
5. **Then record** `scripts/agent_up.sh state <n> phase=... pending=...`.

Order is deliberate: briefs → rings → state. `state <n>` fails before `setup`, and a worker ringing NEED "brief path missing" is answered by re-writing the brief and re-ringing — never by pasting the payload into chat.

Brief skeleton (dev.txt; qe.txt mirrors with qe paths) — the last line is always the worker's ring command, copied verbatim when done:
```
<operator words verbatim>
issue: <n>
role prompt: {repo}/prompts/PROMPT-dev.md
branch: <branch>
scripts/agent_up.sh wake <n> dev nexios-coord "inbox: dev READY — read inbox/dev.md"
```
After any compaction, re-anchor (re-read ANCHOR.md + pwd/runtime/state.json) before acting; never trust a recalled absolute path.

## Parallelism (operator-confirmed)

No worker idles at a handoff — briefing the NEXT task is part of finishing the current one:
- Pipeline lanes: while a fix sits in audit/QE gates, dev starts the next lane's plan/read-only phase (no branch, no code). A parked worker is a coordinator failure, not patience.
- Brief NEXT before the current task completes: QE's verification approach for feature N is briefed while it verifies fix N-1; the auditor takes the plan audit the moment the plan lands.
- Pair plans: for features, dev drafts the implementation plan and QE drafts the verification approach in parallel, so the plan audit cross-checks build-vs-verify coverage before any code.
- Merge prep never jumps gates: worktree creation, LEARNINGS, close, push happen only after audit APPROVED + QE READY are both on file.
- Queued tasks are not rung: a QUEUED brief waits for its trigger (e.g. QE starts #171 work the moment it posts #406 READY, with or without a fresh ring).
- NO IDLE RULE (operator order, overrides queuing caution): when an agent finishes its actual task, the coordinator commands the next pipeline task immediately — an agent never sits idle waiting for gates on someone else's work. If the next phase depends on an unmerged tip, start it STACKED on that tip branch and rebase at merge (rebase first, fold audit hardenings first, never merge out of order)
- BLOCKED-ON-QE RULE (operator order, clarifies the above): gates are not input. If dev's next step needs QE's *output* (verification approach, test patch, evidence, verdict) and the pipeline itself cannot proceed — dev does NOT wait and does NOT stack. The coordinator moves dev to another open issue from the backlog until QE delivers, then dev returns with an explicit re-anchor (room inbox tail + branch state). Stacking is for gate-waits; switching is for input-waits. A worker reporting idle/holding is never a resting state to relay — it is an immediate reassignment trigger; the coordinator assigns the next backlog task in the same turn, without being asked.. Idleness is a coordinator failure, not patience.
