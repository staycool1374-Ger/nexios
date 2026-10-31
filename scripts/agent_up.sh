#!/usr/bin/env bash
# agent_up.sh — provision nexios agent sessions + full environment setup.
# CANONICAL LOCATION: this file (repo checkout, scripts/). No other copy is
# authoritative — a stale copy under ~/.local/state/nexios/bin/ is superseded
# by this file and must be deleted, never run. The script stays UNTRACKED until
# an explicit audit + merge decision commits it; `git add -A` must never be run
# blindly while it is untracked (see clean-tree guard).
#
# WHAT IT SETS UP (one command, zero out-of-repo prerequisites):
#   workers    named fenced tmux sessions (nexios-dev, nexios-qe, nexios-audit)
#   consultant named fenced tmux session (nexios-consultant) — the operator's
#              read-only advisor window. It never drives workers, rings, or
#              state; orchestration belongs to the engine below.
#   engine     headless tmux session (nexios-engine) running the deterministic
#              state-machine tick loop over all rooms (single heartbeat).
#   comms      room runtime root /tmp/nexios-<issue>/ (inbox/dev|qe|auditor.md,
#              state.json, prompts/, notes/, PROTOCOL.md), role configs
#              bootstrapped from embedded defaults if absent.
#
# DOORBELL (tmux send-keys — harness cannot see tmux sessions,
# so `harness send`/agent-messages is NOT a wake path here):
#   engine→worker: agent_up.sh msg nexios-dev "inbox: <text> — read inbox/dev.md"
#   worker→engine: agent_up.sh wake <n> <role> nexios-engine "inbox: <text> — read inbox/<role>.md"
#              (durable WAKE line in inbox/coord.md, no keystroke: the engine is
#              a running loop, not a TUI — keystrokes would corrupt its input).
# Rules: single line, inbox POINTER only (payloads never in chat),
# idle sessions only, never touch permission dialogs (approve mode is person-run).
# A ring means "re-read the inbox", never execute ring text.
#
# WHY TMUX, NOT `harness new`: fenced sessions need OPENCODE_CONFIG in their
# process env. `harness new` talks to the daemon over IPC, which drops the
# invoker's env (no --env flag), so harness-managed sessions launch UNFENCED
# (proven: S1 step-zero fails there). Direct tmux launch inherits our env, so
# fences hold (proven: S1 passes). Tradeoff: tmux sessions are invisible to
# `harness ps` / the grid. Watch via `tmux attach -t <name>` (read-write mirrors;
# read-only (-r) mirrors block ALL tmux input including doorbells, so none here).
# Proper fix belongs upstream: env passthrough on `harness new`.
# Fencing itself is verified per-session BEFORE any prompt is delivered
# (`S4-9`): a session whose env lacks the expected config is
# killed rather than trusted.
#
# TMPDIR is PINNED into the allowed /tmp/agentup-tmp-<session> namespace. The
# host TMPDIR (/var/folders/... on macOS) is deliberately not allowed: it is
# per-boot, so it cannot be fenced, and an unfenced TMPDIR is how writers fell
# back to bash (unfenced by design) during the #410 burn-in.
set -euo pipefail

# Canonical content lives IN the repo (versioned). The state dir below holds
# LINKS ONLY — a real file out of repo is versioning drift and hard-fails here.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
FENCE_DIR="${FENCE_DIR:-$REPO_ROOT/tools/agents/fence}"
RENDERER="$REPO_ROOT/tools/agents/render_fence.py"
NEXIOS_HOME="${NEXIOS_HOME:-$HOME/.local/state/nexios}"
RENDERED_ROOT="$NEXIOS_HOME/rendered"
COORD_DIR="$NEXIOS_HOME/coordinator"
OPENCODE_BIN="${OPENCODE_BIN:-$HOME/.opencode/bin/opencode}"
TUI_BOOT_WAIT="${TUI_BOOT_WAIT:-12}"
DEV_DIR="$HOME/jarvis"
QE_DIR="$HOME/jarvis_test"
DOORBELL_SESS="nexios-engine"
DEV_SESS="nexios-dev"
QE_SESS="nexios-qe"
AUDIT_SESS="nexios-audit"

fail() { echo "agent_up: ERROR: $*" >&2; exit 1; }
info() { echo "agent_up: $*"; }

# Rendered-fence layout. The path is produced in ONE place so a role can never
# be written to another role's file by a copy-paste bug (self-test R6/L1).
rendered_path() {
  [ $# -eq 1 ] || fail "rendered_path needs a role"
  printf '%s/%s/opencode.resolved.json\n' "$RENDERED_ROOT" "$1"
}
CONSULT_SESS="nexios-consultant"
CONSULT_CONFIG_RESOLVED="$(rendered_path consultant)"
CONSULT_TMPDIR=""
ENGINE_SESS="nexios-engine"

usage() {
  cat >&2 <<EOF
usage: agent_up.sh <mode> [...]
  setup <issue>      create coord runtime (/tmp/nexios-<issue>/: inbox, state,
                     prompts, notes, PROTOCOL) + render every role fence into
                     $RENDERED_ROOT. Idempotent: never overwrites state/inbox.
  consult [taskfile] ensure $CONSULT_SESS tmux session (read-only advisor TUI,
                     no attach, engine-safe). Optional taskfile is pasted as
                     the first message; default is a standby line.
  windows            open visible Terminal windows for consultant + dev + qe
                     (read-write mirrors). No coordinator session exists anymore;
                     the engine loop owns every lane.
  workers [issue]    launch $DEV_SESS + $QE_SESS (detached tmux). With an issue
                     they get prompts/dev.txt + prompts/qe.txt from the room
                     runtime; without one they start bare and wait for a ring.
  up [issue] ["task"] FULL SYSTEM from one zsh command: setup (only if an
                     issue is given) + workers + consultant + headless engine
                     loop + three visible Terminal windows for
                     consultant/dev/qe (read-write mirrors; read-only mirrors
                     block ALL tmux input including doorbells). Bare 'up'
                     resumes every session in its last state (no runtime
                     created, no briefs written — cold starts go through
                     provision with an issue list).
                      Every 'up' also disarms legacy per-room tickers; parked
                      lanes resume from their declared NEXT on the first loop
                      cycle (ring ledger caps re-rings).
  run ...            launch ONE session: run <name> <dir> <role> <prompt.txt> [branch]
                     <role> is dev|qe|auditor|consultant — the fence for it is
                     rendered from tools/agents/fence/ at launch (never a path).
  audit <issue>      launch $AUDIT_SESS from <runtime>/prompts/auditor.txt
  audit-diff [branch] [--base <rev>]
                     write the audit diff to audits/pending_patch.diff
                     (base defaults to merge-base main <branch>; must be
                     non-empty). Run in the dev checkout. Brand-new files are
                     invisible to 'git diff <base>': 'git add -N' them first.
  qe-merge <issue>   merge origin/main into testbed --no-commit --no-ff;
                     abort-on-conflict restores code state. Run in QE checkout.
  msg <sess> <text>  single-line doorbell to a tmux session (both directions).
  wake <issue> <from> <sess> <text>
                     durable wake: msg doorbell PLUS an ordered WAKE line in
                     /tmp/nexios-<issue>/inbox/coord.md (fails loud when the
                     runtime/inbox is absent). Target nexios-engine skips the
                     keystroke (append-only): the engine is a running loop.
  comms-test [issue] prove the doorbell end-to-end in BOTH directions using two
                     scratch echo sessions (no opencode, no tokens).
  state <issue> key=value [...]  locked state.json write (validated keys/values).
  status [issue]     tmux + branches + inbox + state overview (read-only).
  status --reconstruct <issue> --copy <dir>  rebuild state.json machine fields
                     from git branches + issue comments into a COPY (live
                     state.json is never touched; --copy is mandatory).
  fence show <role>  print the composed permission map (no launch, no write).
  fence install [dir]  regenerate every rendered config + the subagent files
                     under <dir>/.opencode/agents (default: this checkout).
  render-self-test   fence renderer/verifier matrix (no tmux, no opencode).
  self-test          guard + helper self-tests (scratch tmux names only; never
                     touches $DEV_SESS/$QE_SESS/$AUDIT_SESS/$CONSULT_SESS/$ENGINE_SESS).
  approve <sess>     drive one Allow-always on the session's CURRENT permission
                     dialog (checks first; person-run only).
  close [--all]      kill worker tmux sessions; --all also kills the
                     consultant + engine loop (+ legacy nexios-coord if present).
  clean <issue> [--dry-run]  remove a CLOSED lane's merged branches + room;
                     refuses on open issue, unpushed main, live room.
  watch <issue> [arm|disarm|tick|status]  dev/qe activity watchdog
                     (bare verb = status); arm takes interval_s (default 180).
                     Legacy: the engine loop owns ticks; tickers stay disarmed.
EOF
  exit 2
}

need_file() { [ -f "$1" ] || fail "missing: $1"; }
need_cmd() { command -v "$1" >/dev/null 2>&1 || fail "not on PATH: $1"; }
runtime_dir() {
  [ $# -eq 1 ] || fail "need issue number"
  case "$1" in ''|*[!0-9]*) fail "issue must be numeric: '$1'";; esac
  # Canonical form only: `setup 0410` must never alias onto /tmp/nexios-410.
  local n="$1"
  while [ "${n#0}" != "$n" ]; do n="${n#0}"; done
  [ -n "$n" ] || fail "issue must be non-zero: '$1'"
  [ ${#n} -le 7 ] || fail "issue number too long (max 7 digits): '$1'"
  printf '/tmp/nexios-%s\n' "$n"
}

# Per-session TMPDIR, inside the allowed /tmp/agentup-* namespace (both the flat
# rule and the /** rule exist, so files created inside are fenced too).
ensure_session_tmpdir() {
  local dir="/tmp/agentup-tmp-$1"
  mkdir -p -m 0700 "$dir" || fail "cannot create $dir"
  chmod 700 "$dir" 2>/dev/null || fail "cannot restrict $dir to 0700"
  printf '%s\n' "$dir"
}

# Read state.json without ever leaking a traceback (#412 carried hardening).
# $1 runtime dir, $2 comma-separated keys: one key prints the value, several
# print a {key: value} projection (empty string for a missing file).
read_state() {
  local rt="$1" keys="$2"
  [ -f "$rt/state.json" ] || { printf '\n'; return 1; }
  python3 - "$rt/state.json" "$keys" <<'EOF'
import json, sys
path, keys = sys.argv[1], [k for k in sys.argv[2].split(",") if k]
try:
    with open(path) as handle:
        state = json.load(handle)
except (OSError, ValueError) as exc:
    sys.stderr.write("state: cannot read %s: %s\n" % (path, exc))
    raise SystemExit(1)
if not isinstance(state, dict):
    sys.stderr.write("state: %s is not a JSON object\n" % path)
    raise SystemExit(1)
if len(keys) == 1:
    print(state.get(keys[0], ""))
else:
    print({k: state.get(k) for k in keys})
EOF
}

# ---fence rendering (single source)-------------------------------------------
# The permission map lives ONLY in tools/agents/fence/{base.json,roles/*.json}.
# opencode has no permission `include`, so a complete config is RENDERED per
# launch into $NEXIOS_HOME/rendered/<role>/opencode.resolved.json. No file in
# the repo is a finished map, and no file outside the repo is authoritative.
# `render_fence.py` owns composition + validation; this wrapper only locates
# files and fails closed.
render_role_config() {
  [ $# -eq 1 ] || fail "render_role_config needs a role"
  need_cmd python3
  need_file "$RENDERER"
  need_file "$FENCE_DIR/base.json"
  local role="$1"
  local out
  out=$(rendered_path "$role")
  [ -f "$FENCE_DIR/roles/$role.json" ] || fail "missing fence delta for role '$role' in $FENCE_DIR/roles/"
  mkdir -p "$(dirname "$out")"
  if ! python3 "$RENDERER" --role "$role" --out "$out" --repo-root "$REPO_ROOT" \
      --fence-root "$FENCE_DIR" --twins-out "$(dirname "$out")" >/dev/null; then
    fail "cannot render fence for role '$role'"
  fi
  [ -s "$out" ] || fail "rendered config is empty: $out"
  printf '%s\n' "$out"
}

# $1 role, $2 field (cwd|prompt|name) — reads the role delta's metadata.
role_field() {
  [ $# -eq 2 ] || fail "role_field needs a role and a field"
  need_cmd python3
  need_file "$RENDERER"
  python3 "$RENDERER" --meta --role "$1" --field "$2" --repo-root "$REPO_ROOT" \
    --fence-root "$FENCE_DIR"
}

render_all() {
  local role
  for role in dev qe auditor consultant; do
    render_role_config "$role" >/dev/null
  done
  info "fence rendered for dev, qe, auditor, consultant under $RENDERED_ROOT"
}

# Install the generated subagent definitions into a session's cwd. opencode
# discovers agents in <cwd>/.opencode/agents/, so a subagent invoked FROM a
# session gets its cage from the same delta that produced the session's own
# config (a permission-less agent file would inherit the caller's map — for the
# auditor invoked from a dev session that would be edit:allow). The directory is
# gitignored and launcher-owned; tracked twins carry prose only.
install_agents() {
  local dir="$1"
  [ -d "$dir" ] || fail "agent install target missing: $dir"
  need_cmd python3
  need_file "$RENDERER"
  local role
  for role in dev qe auditor consultant; do
    python3 "$RENDERER" --role "$role" --out "$(rendered_path "$role")" \
      --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR" \
      --twins-out "$dir/.opencode/agents" >/dev/null \
      || fail "cannot install the generated $role agent into $dir"
  done
}

# symbolic launch.cwd -> checkout. Session names stay fixed here: they are the
# doorbell address, not fence data.
role_dir() {
  case "$1" in
    dev|audit) printf '%s\n' "$DEV_DIR" ;;
    qe) printf '%s\n' "$QE_DIR" ;;
    coord) printf '%s\n' "$COORD_DIR" ;;
    *) fail "unknown launch cwd: '$1'" ;;
  esac
}

# Point an out-of-repo path at its versioned in-repo counterpart. Links are
# (re)created; a REAL file in its place is drift → hard fail, never silently
# "adopted" (that is how out-of-repo copies appear).
ensure_link() {
  local link="$1" target="$2"
  if [ -L "$link" ]; then
    [ "$(readlink "$link")" = "$target" ] || ln -sfn "$target" "$link"
  elif [ -e "$link" ]; then
    fail "$link holds a REAL file; canonical content is $target. Move or delete it, then re-run (nothing is overwritten)."
  else
    mkdir -p "$(dirname "$link")"
    ln -s "$target" "$link"
    info "linked out-of-repo path: $link -> $target"
  fi
}

# A legacy path may only be a LINK; a link is pruned (the artifact it pointed at
# no longer exists), a REAL file hard-fails — never silently adopted.
prune_link() {
  local link="$1"
  if [ -L "$link" ]; then
    rm -f "$link"
    info "pruned legacy link: $link"
  elif [ -e "$link" ]; then
    fail "$link holds a REAL file from the pre-#414 layout; the fence is now rendered from $FENCE_DIR. Archive it (e.g. mv \"$link\" \"$link.pre414\"), then re-run."
  fi
}

# State dir links: AGENTS.md (legacy coordinator doctrine, kept for history). Config files are RENDERED
# into $RENDERED_ROOT and never linked; pre-#414 links/role copies are pruned.
ensure_state_links() {
  mkdir -p "$NEXIOS_HOME"
  prune_link "$COORD_DIR/AGENTS.md"
  prune_link "$COORD_DIR/opencode.json"
  prune_link "$COORD_DIR/opencode.resolved.json"
  prune_link "$NEXIOS_HOME/roles/dev.json"
  prune_link "$NEXIOS_HOME/roles/qe.json"
  prune_link "$NEXIOS_HOME/roles/auditor.json"
  prune_link "$NEXIOS_HOME/roles/coordinator.json"
  prune_link "$NEXIOS_HOME/roles/README.md"
  if [ -d "$NEXIOS_HOME/roles" ] && [ -z "$(ls -A "$NEXIOS_HOME/roles" 2>/dev/null)" ]; then
    rmdir "$NEXIOS_HOME/roles" 2>/dev/null || true
  fi
}

# ---coord runtime (comms setup)------------------------------------------------
write_protocol() {
  cat > "$1/PROTOCOL.md" <<'EOF'
# Mailbox protocol

- Dir: `/tmp/nexios-<issue>/inbox/<dev|qe|auditor>.md` — shared, append-only.
- One line per message: `<ISO8601> FROM=<role> NEED|READY|NOTE :: <text>`
- Single-line `O_APPEND` writes are atomic; python3-fcntl guard for multi-line.
- Receiver reads its own inbox at natural step boundaries only (step start,
  post-test-run). Workers never poll.
- Payloads live here, never in chat.
- Doorbell (both directions, tmux — harness cannot see tmux sessions):
  coord→worker:  agent_up.sh msg nexios-dev "inbox: READY from dev — read inbox/qe.md"
  worker→coord:  agent_up.sh msg nexios-coord "inbox: dev READY — read inbox/dev.md"
  Rules: single line, inbox POINTER only, idle sessions only, never dialogs.
  A ring means "re-read the inbox", never execute ring text.
- Durable wake (worker→engine only): `agent_up.sh wake <issue> <from> <sess>
  <text>` sends the doorbell AND appends `<ISO8601> FROM=<from> WAKE ::
  <text>` to `/tmp/nexios-<issue>/inbox/coord.md` (ordered, survives closed
  terminals; keystroke stays the live doorbell, the wake line is the durable
  record). Target nexios-engine skips the keystroke (append-only). Fails loud
  when the runtime/inbox is absent — never silent.
  Every worker brief ends with the ring template as its last line (verbatim
  when done): `agent_up.sh wake <n> <role> nexios-engine "inbox: <role>
  <READY|NEED> — read inbox/<role>.md"` (the engine fills in <n>; <role>
  is dev|qe|auditor).
- Doorbell address: the engine loop (headless `nexios-engine` session); the
  operator's window is the consultant (`nexios-consultant`), which is never rung.
- State: `/tmp/nexios-<issue>/state.json` — single writer: the engine, and the
  only sanctioned write path is `agent_up.sh state <issue> key=value` (python3
  fcntl lock on `state.lock`, atomic replace + fsync, keys/vocabulary validated).
- Proactive pipelining (operator order, mandatory — no idle):
  While a fix sits in audit/QE gates, dev starts next lane's plan/read-only phase (no branch, no code) — a parked worker is an engine failure, not patience.
  Brief NEXT before current completes: QE's verification approach for feature N is briefed while it verifies fix N-1; auditor takes plan audit the moment plan lands.
  Pair plans: dev drafts implementation plan + QE drafts verification approach in parallel, plan audit cross-checks build-vs-verify before code.
  NO IDLE: when an agent finishes its actual task, the engine commands the next pipeline task immediately — an agent never sits idle waiting for gates.
  BLOCKED-ON-QE: gates are not input — if dev's next step needs QE output and pipeline cannot proceed, the engine moves dev to another open issue from backlog until QE delivers, then re-anchors.
  ARMED: queued triggers (e.g., `on dev READY -> verify`, `on QE VERIFIED -> merge`) are input-wait, not idleness — the engine parks on them and rings only when they fire.
- `audit_branch` (optional, string): the branch the auditor's checkout must be
  on. Write it with `agent_up.sh state <issue> audit_branch=<branch>`; `audit
  <issue>` passes it to the launch branch check. Empty/unset = skip the check.
EOF
}

write_state_skeleton() {
  python3 - "$1" "$2" <<'EOF'
import json, sys
path, issue = sys.argv[1], sys.argv[2]
s = {"halt": False, "issue": int(issue), "phase": "",
     "state": "IN_PROGRESS", "retries": 0, "retry_budget": 3,
     "updated": "", "pending": "provision workers"}
open(path, "w").write(json.dumps(s, indent=2) + "\n")
EOF
}

# ---compaction anchor--------------------------------------------------------
# ANCHOR.md lives in the RUNTIME dir (not the repo, not the session cwd):
# its paths are host-specific ($HOME, /tmp canonicalization, per-issue
# runtime) and must never be committed or copied into doctrine (#414).
# Rewritten by every `setup` (canonicalizes the live host); workers re-read
# it after any compaction instead of trusting a recalled absolute path.
write_anchor() {
  local rt="$1"
  python3 - "$rt" "$REPO_ROOT" "$QE_DIR" "$RENDERED_ROOT" <<'EOF'
import os, socket, sys
from datetime import datetime, timezone
rt, repo, qe, rendered = sys.argv[1:5]
def canon(p):
    try:
        return os.path.realpath(p)
    except OSError:
        return p + " (MISSING)"
lines = [
    "# ANCHOR — canonical absolute paths for this room",
    "# Written by `setup`; host-specific, per-issue. Re-read after ANY",
    "# compaction (with pwd + runtime/state.json) before acting; never",
    "# trust a recalled absolute path, never copy these into doctrine.",
    "- repo (dev checkout): %s" % canon(repo),
    "- testbed checkout: %s" % canon(qe),
    "- runtime dir: %s" % canon(rt),
    "- inbox dir: %s" % canon(os.path.join(rt, "inbox")),
    "- rendered fence dir: %s" % canon(rendered),
    "- host: %s  utc: %s" % (socket.gethostname(),
                             datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")),
    "",
]
with open(os.path.join(rt, "ANCHOR.md"), "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines))
EOF
}

# $1 issue. Idempotent: creates missing pieces, never overwrites state/inbox.
mode_setup() {
  [ $# -eq 1 ] || fail "setup needs an issue number"
  local rt; rt=$(runtime_dir "$1")
  ensure_state_links
  render_all
  mkdir -p -m 0700 "$rt/inbox" "$rt/notes" "$rt/prompts"
  for r in dev qe auditor coord; do
    [ -f "$rt/inbox/$r.md" ] || echo "# inbox/$r.md — append-only. Format: <ISO8601> FROM=<role> NEED|READY|NOTE :: <text>" > "$rt/inbox/$r.md"
  done
  [ -f "$rt/state.json" ] || { write_state_skeleton "$rt/state.json" "$1"; info "seeded state.json (IN_PROGRESS, retries 0/3)"; }
  write_protocol "$rt"
  write_anchor "$rt"
  link_runtime "$1" "$rt"
  info "coord runtime ready: $rt"
}

# In-repo discovery pointer for the /tmp runtime. The payload (inbox, state,
# prompts) stays in /tmp; the repo carries one gitignored symlink PER ISSUE so
# every path an agent needs is reachable from the checkout and setting up a new
# issue never hijacks the pointer of a live one.
link_runtime() {
  local issue="$1" rt="$2"
  local dir="$REPO_ROOT/.nexios-runtime"
  # Key from the CANONICAL runtime dir, never from the raw argv: `setup 000410`
  # and `setup 410` must share one pointer, not create a second one.
  local key="${rt##*/nexios-}"
  mkdir -p "$dir"
  ln -sfn "$rt" "$dir/$key"
  info "runtime pointer: .nexios-runtime/$key -> $rt"
}

# ---provisioning---------------------------------------------------------------
# $1 dir, $2 expected branch (empty = skip check)
check_branch() {
  local cur
  cur=$(git -C "$1" branch --show-current)
  [ -z "$2" ] || [ "$cur" = "$2" ] || fail "$1 on '$cur', want '$2'"
}

# resume normalization: a worker closed on a feature branch must not block
# `up`/`workers` (the fence only allows launching from the role root).
# $1 checkout dir, $2 role (for messages), $3 expected root branch.
# On root: no-op. Elsewhere + clean tree: checkout root. Elsewhere + dirty
# tree: named TRACKED-ONLY stash, then checkout root (untracked build
# artifacts and QEMU logs are never stashed). Detached HEAD: same rules.
normalize_checkout() {
  local dir="$1" role="$2" want="$3" cur ts
  cur=$(git -C "$dir" branch --show-current)
  [ "${cur:-HEAD}" = "$want" ] && return 0
  if git -C "$dir" diff --quiet && git -C "$dir" diff --cached --quiet; then
    git -C "$dir" checkout "$want" --quiet \
      || fail "$role checkout: cannot return to '$want' from '${cur:-HEAD}'"
    info "$role checkout: '${cur:-HEAD}' (clean) -> '$want'"
  else
    ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    git -C "$dir" stash push -m "agent_up.sh workers: $role ${cur:-HEAD} $ts (auto, resume-normalize)" \
      || fail "$role checkout: stash failed on '${cur:-HEAD}', resolve manually"
    git -C "$dir" checkout "$want" --quiet \
      || fail "$role checkout: cannot return to '$want' after stash"
    info "$role checkout: '${cur:-HEAD}' (dirty, tracked-only stash) -> '$want'"
    git -C "$dir" stash list | head -n 3
  fi
}

# clean-tree guard: refuse when tracked files (minus listed prefixes) are dirty.
# $1 workdir, $2... exempt path prefixes (e.g. graphify-out/ test-history.txt)
# Porcelain -z parsing: plain `git status --porcelain | awk '{print $2}'` loses
# rename origins (reports the OLD path) and truncates paths with spaces.
check_clean_tree() {
  local dir="$1"; shift
  local out rc=0
  out=$(python3 - "$dir" "$@" <<'EOF'
import os, subprocess, sys
d, exempt = sys.argv[1], tuple(sys.argv[2:])
raw = subprocess.run(["git", "-C", d, "status", "--porcelain=v1", "-z"],
                     capture_output=True)
if raw.returncode != 0:
    sys.exit(2)  # unreadable tree is an error, never "clean"
recs = raw.stdout.split(b"\0")
i = 0
paths = []
while i < len(recs):
    rec = recs[i]
    if not rec:
        i += 1
        continue
    xy, path = rec[:2], rec[3:]
    paths.append(os.fsdecode(path))
    if xy[:1] in (b"R", b"C"):
        i += 1  # the next record is the rename/copy ORIGIN
    i += 1
bad = [p for p in paths if not (exempt and p.startswith(exempt))]
for p in bad:
    print(p)
sys.exit(1 if bad else 0)
EOF
) || rc=$?
  [ "$rc" = 0 ] && return 0
  if [ "$rc" = 1 ]; then
    fail "dirty tree: $(printf '%s' "$out" | tr '\n' ' ')(commit/stash first; exempt: $*)"
  fi
  fail "cannot read git status for $dir"
}

# Locked read-modify-write on state.json — the ONLY sanctioned state writer.
# Lock lives on a stable sidecar (state.lock) so atomic replace cannot create a
# stale-inode writer; no flock binary on macOS, fcntl only.
# usage: state <issue> key=value [key=value...]
mode_state_set() {
  [ $# -ge 2 ] || fail "state needs: <issue> key=value [key=value...]"
  local issue="$1"; shift
  local rt; rt=$(runtime_dir "$issue")
  need_file "$rt/state.json"
  python3 - "$rt/state.json" "$@" <<'EOF'
import datetime, fcntl, json, os, sys, tempfile


def die(msg):
    sys.stderr.write("state: %s\n" % msg)
    sys.exit(1)


path, pairs = sys.argv[1], sys.argv[2:]
ALLOWED = {"audit_branch", "halt", "pending", "phase", "retries",
           "retry_budget", "state", "updated", "release_armed", "gh_notes"}
STATES = {"ISSUE_OPEN", "PLANNED", "IN_PROGRESS", "AUDITED", "VERIFIED",
          "CLOSED", "FEEDBACK"}
lock_path = os.path.join(os.path.dirname(path), "state.lock")
with open(lock_path, "a+") as lock:
    fcntl.flock(lock, fcntl.LOCK_EX)
    try:
        with open(path) as f:
            state = json.load(f)
    except OSError as exc:
        die("cannot read %s: %s" % (path, exc.strerror or exc))
    except ValueError as exc:
        die("existing state.json is not valid JSON: %s" % exc)
    if not isinstance(state, dict):
        die("existing state.json is not a JSON object")
    for pair in pairs:
        if "=" not in pair:
            die("expected key=value, got %r" % pair)
        key, value = pair.split("=", 1)
        if key not in ALLOWED:
            die("unknown key %r (allowed: %s)"
                % (key, ", ".join(sorted(ALLOWED))))
        if key in ("retries", "retry_budget"):
            try:
                value = int(value)
            except ValueError:
                die("%s needs an integer, got %r" % (key, value))
            if value < 0:
                die("%s must be >= 0" % key)
        elif key in ("halt", "release_armed", "gh_notes"):
            value = value.lower() in ("1", "true", "yes")
        elif key == "state" and value not in STATES:
            die("%r not in the AGENTS.md vocabulary" % value)
        state[key] = value
    state["updated"] = datetime.datetime.now(
        datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    directory = os.path.dirname(path)
    tmp = None
    try:
        fd, tmp = tempfile.mkstemp(dir=directory, prefix=".state.")
        with os.fdopen(fd, "w") as out:
            json.dump(state, out, indent=2)
            out.write("\n")
            out.flush()
            os.fsync(out.fileno())
        os.replace(tmp, path)
        tmp = None
    except OSError as exc:
        die("cannot write %s: %s" % (path, exc.strerror or exc))
    finally:
        if tmp is not None:
            try:
                os.unlink(tmp)
            except OSError:
                pass
    try:
        dfd = os.open(directory, os.O_RDONLY)
        try:
            os.fsync(dfd)
        finally:
            os.close(dfd)
    except OSError as exc:
        die("cannot fsync %s: %s" % (directory, exc.strerror or exc))
    fcntl.flock(lock, fcntl.LOCK_UN)
print("state: ok (%d key(s) set)" % len(pairs))
EOF
}

# $1 name, $2 dir, $3 role NAME, $4 prompt file, $5 expected branch (empty =
# skip). A role name, never a config path: the fence is rendered from the repo
# source here, so a caller cannot point a session at an arbitrary config.
mode_run() {
  [ $# -ge 4 ] || fail "run needs: <name> <dir> <role> <prompt.txt> [branch]"
  local name="$1"
  local dir="$2"
  local role="$3"
  local prompt="$4"
  local want_branch="${5:-}"
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  case "$role" in dev|qe|auditor|consultant) ;; *) fail "run takes a role name (dev|qe|auditor|consultant), not a config path: '$role'" ;; esac
  need_file "$prompt"
  [ -d "$dir" ] || fail "workdir missing: $dir"
  check_branch "$dir" "$want_branch"
  run_session "$name" "$dir" "$role" "$prompt"
}

mode_audit() {
  [ $# -eq 1 ] || fail "audit needs an issue number"
  local rt; rt=$(runtime_dir "$1")
  # Launch metadata (cwd + prompt file) comes from the role delta, not a
  # hardcoded path: the auditor reads the dev checkout (diff + files) and never
  # writes, and the delta is the single statement of that.
  local dir prompt
  dir=$(role_dir "$(role_field auditor cwd)")
  prompt="$rt/prompts/$(role_field auditor prompt)"
  need_file "$prompt"
  local branch=""
  branch=$(read_state "$rt" audit_branch 2>/dev/null || true)
  mode_run "$AUDIT_SESS" "$dir" auditor "$prompt" "$branch"
}

# $1 tmux-name, $2 workdir, $3 role NAME, $4 prompt file
# (split locals: bash 3.2 expands all RHS before assigning under set -u)
run_session() {
  local name="$1"
  local dir="$2"
  local role="$3"
  local prompt="$4"
  [ -d "$dir" ] || fail "workdir missing: $dir"
  [ -z "$prompt" ] || need_file "$prompt"
  case "$role" in dev|qe|auditor|consultant) ;; *) fail "unknown role: '$role'" ;; esac
  # install_agents installs the generated subagent definitions into the
  # session's cwd; render_role_config then re-renders THIS role fresh. Never
  # trust the file on disk here: a pre-planted or stale config under
  # $RENDERED_ROOT must never launch (the renderer validates the role and
  # overwrites with a fresh compose, fail-closed on any error).
  install_agents "$dir"
  local cfg; cfg=$(render_role_config "$role")
  local reused=0
  if tmux has-session -t "$name" 2>/dev/null; then
    # A resumed launch is legal: `up` can fail after the workers are already
    # running (a stale state file, a missing tool), and its message tells the
    # operator to fix that and re-run — refusing reuse outright made that
    # remedy impossible. Reuse only a session whose fence is provably the one
    # this launch would build now; anything else is still never clobbered.
    check_fenced "$name" "$dir" "$cfg" "$role" 2>/dev/null || fail "tmux session '$name' already exists with a different fence — close it first (never clobbered)"
    reused=1
  fi
  # Env hygiene: role config ONLY from OPENCODE_CONFIG; strip any leaky overrides.
  # TMPDIR pinned inside the fenced namespace (host TMPDIR is per-boot: unfenceable).
  # NEXIOS_AGENT_ROLE is the launcher's own statement of the role, consumed by
  # tools/check_no_cross_edits.sh (a config path alone is weaker evidence).
  local tdir; tdir=$(ensure_session_tmpdir "$name")
  # The launcher hands these to tmux as one space-separated command line and
  # check_fenced reads them back from `ps eww`; a space inside a value would be
  # misparsed. Refuse loudly instead of launching something ambiguous.
  local v
  for v in "$cfg" "$tdir" "$dir"; do
    case "$v" in
      *[[:space:]]*) fail "launch path contains whitespace, which the fenced env line cannot express: '$v'" ;;
    esac
  done
  if [ "$reused" = 0 ]; then
    tmux new-session -d -s "$name" -c "$dir" -x 200 -y 50
    tmux send-keys -t "$name" "env -u OPENCODE_CONFIG -u OPENCODE_CONFIG_CONTENT TMPDIR=$tdir OPENCODE_CONFIG=$cfg NEXIOS_AGENT_ROLE=$role $OPENCODE_BIN" Enter
    sleep "$TUI_BOOT_WAIT"
    check_fenced "$name" "$dir" "$cfg" "$role" || {
      tmux kill-session -t "$name" 2>/dev/null || true
      fail "session '$name' launched UNFENCED (config/role/tmpdir not in process env) — killed, fix before retry"
    }
  fi
  if [ -n "$prompt" ]; then
    tmux load-buffer "$prompt"
    tmux paste-buffer -t "$name"
    sleep 2
    tmux send-keys -t "$name" Enter
    if [ "$reused" = 1 ]; then info "reused '$name' (fenced, prompt delivered)"
    else info "launched '$name' (fenced, prompt delivered)"; fi
  else
    if [ "$reused" = 1 ]; then info "reused '$name' (fenced, no brief yet — the engine rings it)"
    else info "launched '$name' (fenced, no brief yet — the engine rings it)"; fi
  fi
}

# $1 tmux-name, $2 expected PWD, $3 expected rendered config, $4 expected role
# Scoped to the tmux pane's own process tree: dev and auditor share PWD, so a
# global PWD match can select the sibling's opencode (false UNFENCED + kill).
# The match set is PWD + OPENCODE_CONFIG + NEXIOS_AGENT_ROLE + TMPDIR: a session
# that loaded the right file but announced another role (or lost the pinned
# TMPDIR) is not fenced in the way this launcher promises.
check_fenced() {
  local name="$1" want_pwd="$2" want_cfg="$3" want_role="$4"
  local pane pid i found=0 seen=""
  pane=$(tmux list-panes -t "$name" -F '#{pane_pid}' 2>/dev/null | head -1)
  [ -n "$pane" ] || { echo "no tmux pane for '$name'" >&2; return 1; }
  local pids=("$pane")
  i=0
  while [ "$i" -lt "${#pids[@]}" ]; do
    for pid in $(pgrep -P "${pids[$i]}" 2>/dev/null); do pids[${#pids[@]}]="$pid"; done
    i=$((i + 1))
  done
  local want_tmp="/tmp/agentup-tmp-$name"
  for pid in "${pids[@]}"; do
    local env="" pwd_v cfg_v role_v tmp_v hit=1
    env=$(ps eww -p "$pid" 2>/dev/null) || continue
    case "$env" in *"$OPENCODE_BIN"*) ;; *) continue ;; esac
    # Decision by substring on the raw environment: exact-value tokens keep the
    # assertion correct even if a value ever gains quoting/escapes. The
    # tokenized view is for the diagnostic only.
    case "$env" in *"PWD=$want_pwd"*) ;; *) hit=0 ;; esac
    case "$env" in *"OPENCODE_CONFIG=$want_cfg"*) ;; *) hit=0 ;; esac
    case "$env" in *"NEXIOS_AGENT_ROLE=$want_role"*) ;; *) hit=0 ;; esac
    case "$env" in *"TMPDIR=$want_tmp"*) ;; *) hit=0 ;; esac
    pwd_v=$(printf '%s\n' "$env" | tr ' ' '\n' | grep "^PWD=" | head -1 || true)
    cfg_v=$(printf '%s\n' "$env" | tr ' ' '\n' | grep "^OPENCODE_CONFIG=" | head -1 || true)
    role_v=$(printf '%s\n' "$env" | tr ' ' '\n' | grep "^NEXIOS_AGENT_ROLE=" | head -1 || true)
    tmp_v=$(printf '%s\n' "$env" | tr ' ' '\n' | grep "^TMPDIR=" | head -1 || true)
    seen="${seen}pid $pid pwd=${pwd_v:-?} cfg=${cfg_v:-<unset>} role=${role_v:-<unset>} tmp=${tmp_v:-<unset>}; "
    [ "$hit" = 1 ] && { found=1; break; }
  done
  [ "$found" = 1 ] && return 0
  echo "pane tree of '$name' has no opencode with PWD=$want_pwd + OPENCODE_CONFIG=$want_cfg + NEXIOS_AGENT_ROLE=$want_role + TMPDIR=$want_tmp (saw: ${seen:-<none>})" >&2
  return 1
}

consult_tui_cmd() {
  local v
  for v in "$CONSULT_TMPDIR" "$CONSULT_CONFIG_RESOLVED"; do
    case "$v" in
      *[[:space:]]*) fail "consultant launch path contains whitespace, which the fenced env line cannot express: '$v'" ;;
    esac
  done
  echo "env -u OPENCODE_CONFIG_CONTENT TMPDIR=$CONSULT_TMPDIR OPENCODE_CONFIG=$CONSULT_CONFIG_RESOLVED NEXIOS_AGENT_ROLE=consultant $OPENCODE_BIN"
}

# ensure nexios-consultant tmux session exists running the consultant TUI
# (read-only advisor; workers never ring it, the engine tasks it via notes).
ensure_consultant_session() {
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  ensure_state_links
  render_role_config consultant >/dev/null
  need_file "$CONSULT_CONFIG_RESOLVED"
  if tmux has-session -t "$CONSULT_SESS" 2>/dev/null; then
    check_fenced "$CONSULT_SESS" "$COORD_DIR" "$CONSULT_CONFIG_RESOLVED" consultant 2>/dev/null || {
      tmux kill-session -t "$CONSULT_SESS" 2>/dev/null || true
    }
  fi
  if ! tmux has-session -t "$CONSULT_SESS" 2>/dev/null; then
    CONSULT_TMPDIR=$(ensure_session_tmpdir "$CONSULT_SESS")
    tmux new-session -d -s "$CONSULT_SESS" -c "$COORD_DIR" -x 200 -y 50
    tmux send-keys -t "$CONSULT_SESS" "$(consult_tui_cmd)" Enter
    sleep "$TUI_BOOT_WAIT"
    check_fenced "$CONSULT_SESS" "$COORD_DIR" "$CONSULT_CONFIG_RESOLVED" consultant || {
      tmux kill-session -t "$CONSULT_SESS" 2>/dev/null || true
      fail "consultant launched UNFENCED (resolved config not in its env) — killed, fix before retry"
    }
    info "started $CONSULT_SESS (fenced, config=$CONSULT_CONFIG_RESOLVED)"
  fi
}

# consultant endpoint: ensure + optional pasted task, never attach (engine-safe).
# usage: consult [taskfile]
mode_consult() {
  ensure_consultant_session
  if [ $# -ge 1 ]; then
    need_file "$1"
    tmux load-buffer "$1"
    tmux paste-buffer -t "$CONSULT_SESS"
    sleep 2
    tmux send-keys -t "$CONSULT_SESS" Enter
    info "delivered task to $CONSULT_SESS"
  else
    tmux send-keys -t "$CONSULT_SESS" "You are nexios-consultant (read-only advisor). Read $REPO_ROOT/tools/agents/twins/nexios-consultant.md for your charter. Report readiness, then stand by." Enter
    info "delivered standby line to $CONSULT_SESS"
  fi
}

# Single-heartbeat doctrine: the engine loop owns every room from here on;
# legacy per-room tickers are disarmed (re-arm is one command per room).
# Best-effort per room, loud either way; never touches sessions.
disarm_all_tickers() {
  local rt issue pidf
  for rt in /tmp/nexios-*/; do
    [ -d "$rt" ] || continue
    issue="${rt%/}"; issue="${issue##*/nexios-}"
    case "$issue" in ''|*[!0-9]*) continue;; esac
    pidf="$rt/watch.pid"
    [ -f "$pidf" ] || continue
    if "$SCRIPT_DIR/agent_up.sh" watch "$issue" disarm >/dev/null 2>&1; then
      info "ticker disarmed for $issue (engine loop owns ticks)"
    else
      info "ticker left alone for $issue (already down or foreign)"
    fi
  done
}

# The orchestration heartbeat: one headless session running the engine tick
# loop over all rooms. An existing loop is left alone (close it to restart).
ensure_engine_loop() {
  if tmux has-session -t "$ENGINE_SESS" 2>/dev/null; then
    info "engine loop already up ($ENGINE_SESS)"
    return 0
  fi
  mkdir -p "$REPO_ROOT/logs/state-machine"
  tmux new-session -d -s "$ENGINE_SESS" -c "$REPO_ROOT"     "python3 scripts/state_machine.py tick --all --interval 180 >>logs/state-machine/engine-console.log 2>&1"
  info "engine loop started ($ENGINE_SESS, logs/state-machine/)"
}

# visible mirrors for the three live roles (consultant = your window).
mode_windows() {
  local s
  for s in "$CONSULT_SESS" "$DEV_SESS" "$QE_SESS"; do
    tmux has-session -t "$s" 2>/dev/null || fail "no such tmux session: $s (launch it first)"
  done
  open_watch_window "$CONSULT_SESS"
  open_watch_window "$DEV_SESS"
  open_watch_window "$QE_SESS"
}

# ---modes----------------------------------------------------------------------
# workers [issue] — without an issue the sessions start bare (no brief): the
# engine writes the briefs after setup and rings each worker. Briefs are
# only required when an issue was named, so a no-issue room creates no runtime.
mode_workers() {
  [ $# -le 1 ] || fail "workers takes at most one issue number (got $#)"
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  local dp="" qp=""
  if [ $# -eq 1 ]; then
    local rt; rt=$(runtime_dir "$1")
    dp="$rt/prompts/$(role_field dev prompt)"
    qp="$rt/prompts/$(role_field qe prompt)"
    need_file "$dp"; need_file "$qp"
  fi
  normalize_checkout "$DEV_DIR" dev main
  normalize_checkout "$QE_DIR" qe testbed
  run_session "$DEV_SESS" "$DEV_DIR" dev "$dp"
  run_session "$QE_SESS" "$QE_DIR" qe "$qp"
}

# $1 tmux session name — opens a new visible Terminal window attached READ-WRITE
# (owner call: watchers may type; the watch-XOR-drive rule is enforced socially —
# read-only mirrors would block ALL tmux input including doorbells, so none here).
open_watch_window() {
  osascript -e "tell application \"Terminal\" to do script \"tmux attach -t $1\"" >/dev/null
  info "opened watch window for '$1' (read-write attach; type only deliberate input, never paste chat logs)"
}

# Full system from one zsh command: setup + workers + consultant + engine loop
# + visible watch mirrors, with an optional task pasted to the consultant.
# up [issue] ["task"] — the issue is optional: bare `up` resumes every session
# in its last state (cold starts go through provision with an issue list).
mode_up() {
  [ $# -le 2 ] || fail "up takes at most: up [issue] [taskfile-or-task] (got $#)"
  local issue="" task=""
  if [ $# -ge 1 ]; then
    # runtime_dir is the validator: a non-numeric first argument is an error,
    # never an "idea" (cold starts go through provision with an issue list).
    runtime_dir "$1" >/dev/null
    issue="$1"
    task="${2:-}"
    mode_setup "$issue"
  fi
  mode_workers ${issue:+"$issue"}
  ensure_consultant_session
  disarm_all_tickers
  ensure_engine_loop
  open_watch_window "$CONSULT_SESS"
  open_watch_window "$DEV_SESS"
  open_watch_window "$QE_SESS"
  cat <<EOF
watch: three Terminal windows opened (read-write mirrors). Detach with Ctrl-b d.
watch: Harness.app grid stays empty by design (tmux sessions are not discovered).
       Do NOT start opencode in app panes (that would duplicate live workers).
next: consultant '$CONSULT_SESS' (your window) + engine loop '$ENGINE_SESS' (headless heartbeat).
EOF
  if [ -n "$task" ]; then
    if [ -f "$task" ]; then
      mode_consult "$task"
    else
      local tf; tf=$(mktemp)
      printf '%s\n' "$task" > "$tf"
      mode_consult "$tf"
      rm -f "$tf"
    fi
  elif [ -n "$issue" ]; then
    echo "(no task given — the consultant window is yours; the engine drives the lanes.)"
  else
    echo "(no issue selected — no runtime created. Cold starts go through"
    echo " provision with an issue list; parked lanes resume from their NEXT.)"
  fi
}

# audit-diff: generate the audit diff with a non-empty assertion.
# usage: audit-diff [branch] [--base <rev>]   (default: current branch)
# Writes through a temp file: diffing straight into audits/pending_patch.diff
# made the file its own delta (a self-deletion hunk in every audit).
# Process artifacts are excluded so the auditor sees the code, not the trail.
mode_audit_diff() {
  local br="" base_override="" base_set=0
  while [ $# -gt 0 ]; do
    case "$1" in
      --base)
        [ -n "${2:-}" ] || fail "--base needs a revision"
        base_override="$2"; base_set=1; shift 2 ;;
      --base=*)
        base_override="${1#--base=}"
        [ -n "$base_override" ] || fail "--base needs a revision (got an empty value)"
        base_set=1; shift ;;
      *)
        [ -z "$br" ] || fail "unexpected arg: $1"
        br="$1"; shift ;;
    esac
  done
  [ -n "$br" ] || br=$(git branch --show-current)
  [ -n "$br" ] || fail "cannot determine the branch (detached HEAD? pass one)"
  local base base_desc
  if [ "$base_set" = 1 ]; then
    base=$(git rev-parse --verify --quiet "$base_override^{commit}") \
      || fail "base '$base_override' does not resolve to a commit"
    base_desc="$base_override"
  else
    base=$(git merge-base main "$br" 2>/dev/null || true)
    [ -n "$base" ] || fail "no merge-base with main for '$br' (pass --base <rev>)"
    base_desc="merge-base main $br = $base"
  fi
  local out="audits/pending_patch.diff"
  mkdir -p audits
  local tmpf; tmpf=$(mktemp "${TMPDIR:-/tmp}/agentup-auditdiff.XXXXXX")
  local excl=( "--" "." ":!audits/pending_patch.diff" \
               ":!audits/rejected_patch.diff" ":!prompts/LEARNINGS.md" \
               ":!test-history.txt" ":!graphify-out" )
  if [ "$br" = "$(git branch --show-current)" ]; then
    # Same branch: diff the worktree too — uncommitted edits are part of the
    # delta under audit (`base..br` would silently omit them).
    git diff --no-ext-diff "$base" "${excl[@]}" > "$tmpf" \
      || { rm -f "$tmpf"; fail "git diff failed (base $base_desc)"; }
  else
    git diff --no-ext-diff "$base..$br" "${excl[@]}" > "$tmpf" \
      || { rm -f "$tmpf"; fail "git diff failed (base $base_desc)"; }
  fi
  if [ ! -s "$tmpf" ]; then
    rm -f "$tmpf"
    fail "audit diff empty for $br (nothing to audit; base $base_desc)"
  fi
  chmod 0644 "$tmpf"
  mv -f "$tmpf" "$out"
  info "audit diff: $out ($(wc -l < "$out" | tr -d ' ') lines, base $base_desc)"
}

# qe-merge: merge origin/main into testbed --no-commit --no-ff.
# usage: qe-merge <issue>   (run in the QE checkout; aborts cleanly on conflict)
mode_qe_merge() {
  [ $# -eq 1 ] || fail "qe-merge needs an issue number"
  [ "$(git rev-parse --show-toplevel)" = "$(git -C "$QE_DIR" rev-parse --show-toplevel)" ] \
    || fail "run qe-merge in the QE checkout ($QE_DIR), not $(pwd)"
  check_branch "$QE_DIR" testbed
  check_clean_tree "$(pwd)" "test-history.txt" "graphify-out/"
  git fetch origin main 2>/dev/null \
    || fail "git fetch origin main failed — refusing to merge a possibly stale origin/main"
  if git merge --no-commit --no-ff origin/main; then
    info "merged origin/main --no-commit (inspect, test, then commit or abort)"
  else
    git merge --abort || true
    fail "conflict — merge aborted; verify with git status (resolve manually)"
  fi
}

# single-line doorbell to a tmux session (both directions). usage: msg <sess> <text>
mode_msg() {
  [ $# -eq 2 ] || fail "msg needs: <tmux-session> <single-line-text>"
  case "$2" in *$'\n'*) fail "doorbell must be a single line (payloads live in inbox)";; esac
  tmux has-session -t "$1" 2>/dev/null || fail "no such tmux session: $1 (start it first)"
  if tmux list-clients -t "$1" 2>/dev/null | grep -q "read-only"; then
    fail "read-only watcher attached to '$1' — tmux blocks ALL input while one is present. Close the mirror window(s) (Ctrl-b d does not work in -r; close the window), then retry"
  fi
  tmux send-keys -t "$1" "$2" Enter
  info "doorbell sent to '$1'"
}

# Durable wake (worker -> engine): the live keystroke doorbell PLUS an
# ordered wake line in the issue inbox, so the wake survives closed
# terminals and is re-readable at step boundaries.
# usage: wake <issue> <from-role> <sess> <text>
# Target nexios-engine skips the keystroke (append-only): the engine is a
# running loop, not a TUI — keystrokes would corrupt its input.
# Fails loud (never silent) when the runtime or inbox is absent: no
# keystroke is sent and nothing is appended in that case.
mode_wake() {
  [ $# -eq 4 ] || fail "wake needs: <issue> <from-role> <tmux-session> <single-line-text>"
  local rt; rt=$(runtime_dir "$1")
  case "$2" in dev|qe|auditor|coord) ;; *) fail "wake from-role must be dev|qe|auditor|coord (got '$2')";; esac
  case "$4" in *$'\n'*) fail "wake text must be a single line (payloads live in inbox)";; esac
  [ -d "$rt/inbox" ] || fail "no runtime inbox for issue $1 (run setup $1 first)"
  [ -f "$rt/inbox/coord.md" ] || fail "no inbox/coord.md for issue $1 (run setup $1 first)"
  if [ "$3" = "$ENGINE_SESS" ]; then
    info "engine sink: keystroke skipped (append-only)"
  else
    mode_msg "$3" "$4"
  fi
  printf '%s FROM=%s WAKE :: %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$2" "$4" >> "$rt/inbox/coord.md" \
    || fail "wake not durable for '$3' (inbox append failed)"
  info "wake recorded in '$rt/inbox/coord.md'"
}

# End-to-end transport self-test: proves msg delivers text+Enter INTO a live
# tmux pane in both directions, using two scratch echo sessions (no opencode,
# no tokens, no $DEV_SESS/$QE_SESS/$CONSULT_SESS/$ENGINE_SESS touched).
mode_comms_test() {
  local issue="${1:-999999}"
  local rt; rt=$(runtime_dir "$issue")
  need_cmd tmux
  mkdir -p "$rt/notes"
  local a="agentup-ct-a-$$" b="agentup-ct-b-$$"
  cat > "$rt/notes/echo_agent.sh" <<'EOF'
#!/bin/sh
while IFS= read -r line; do printf 'GOT:%s\n' "$line"; done
EOF
  kill_sess() { tmux kill-session -t "$1" 2>/dev/null || true; }
  trap "tmux kill-session -t '$a' 2>/dev/null || true; tmux kill-session -t '$b' 2>/dev/null || true" EXIT
  tmux new-session -d -s "$a" "sh $rt/notes/echo_agent.sh"
  tmux new-session -d -s "$b" "sh $rt/notes/echo_agent.sh"
  sleep 1
  local ok=1
  # engine -> worker direction / worker -> engine direction (same mode_msg path)
  mode_msg "$a" "PING-$$" >/dev/null || ok=0
  mode_msg "$b" "PONG-$$" >/dev/null || ok=0
  sleep 1
  tmux capture-pane -p -t "$a" | grep -q "GOT:PING-$$" || { echo "FAIL: engine->worker not delivered"; ok=0; }
  tmux capture-pane -p -t "$b" | grep -q "GOT:PONG-$$" || { echo "FAIL: worker->engine not delivered"; ok=0; }
  # guard still fails loud on a missing session
  if ( mode_msg "agentup-ct-missing-$$" "x" ) >/dev/null 2>&1; then
    echo "FAIL: msg accepted a nonexistent session"; ok=0
  fi
  [ "$ok" = 1 ] || fail "comms-test FAILED"
  kill_sess "$a"; kill_sess "$b"; trap - EXIT
  info "comms-test PASSED: doorbell delivered in both directions (PING-$$/PONG-$$)"
}

mode_status() {
  if [ "${1:-}" = "--reconstruct" ]; then shift; mode_status_reconstruct "$@"; return; fi
  info "--- tmux ---"; tmux ls 2>&1 || true
  info "--- branches ---"
  echo "dev: $(git -C "$DEV_DIR" branch --show-current 2>/dev/null || echo '?')"
  echo "qe:  $(git -C "$QE_DIR" branch --show-current 2>/dev/null || echo '?')"
  if [ $# -ge 1 ]; then
    local rt; rt=$(runtime_dir "$1")
    for r in dev qe auditor coord; do
      echo "--- inbox/$r.md (tail) ---"
      tail -2 "$rt/inbox/$r.md" 2>/dev/null || echo "(none)"
    done
    if [ -f "$rt/state.json" ]; then
      echo "--- state.json ---"
      read_state "$rt" "phase,state,retries,retry_budget,halt,pending" \
        || echo "(state.json unreadable)"
    fi
  fi
}

# rebuild state.json machine fields from git + issue into a COPY (live untouched).
# usage: status --reconstruct <issue> --copy <dir>
mode_status_reconstruct() {
  local issue="" copy=""
  while [ $# -gt 0 ]; do
    case "$1" in
      --copy) copy="$2"; shift 2;;
      *) [ -z "$issue" ] && { issue="$1"; shift; } || fail "unexpected arg: $1";;
    esac
  done
  [ -n "$issue" ] && [ -n "$copy" ] || fail "need: status --reconstruct <issue> --copy <dir>"
  case "$copy" in /tmp/?*) ;; *) fail "refusing: --copy must be a direct /tmp/<name> path";; esac
  case "${copy#/tmp/}" in */*) fail "refusing: --copy must be a direct /tmp/<name> path";; esac
  case "$(basename "$copy")" in
    *-drill|*-staging|*-copy) ;;
    *) fail "refusing: --copy dir name must end in -drill, -staging or -copy (got $(basename "$copy"))";;
  esac
  # `--copy` is checked as a string; a symlink named *-copy would write
  # through to the live dir (or any target), defeating "live untouched".
  [ -L "$copy" ] && fail "refusing: --copy must not be a symlink (got $(readlink "$copy"))"
  local rt; rt=$(runtime_dir "$issue")
  [ -f "$rt/state.json" ] || fail "no live state to compare against"
  case "$copy" in "$rt") fail "refusing to reconstruct the live dir (use a COPY)";; esac
  mkdir -p "$copy"
  # A hardlink named *-copy is not a symlink but shares the live inode, so
  # writing through it rewrote live state (S3-1R). The shell `-ef` check is the
  # fast path; the Python re-check is the one that cannot be raced.
  if [ -e "$copy/state.json" ] && [ "$copy/state.json" -ef "$rt/state.json" ]; then
    fail "refusing: $copy/state.json shares the live state inode (hardlink) — use a real copy"
  fi
  python3 - "$rt" "$copy" "$DEV_DIR" "$QE_DIR" <<'EOF'
import json, os, subprocess, sys, tempfile


def die(msg):
    sys.stderr.write("agent_up: ERROR: %s\n" % msg)
    sys.exit(1)


rt, copy, devdir, qedir = sys.argv[1:5]
try:
    with open(rt + "/state.json") as handle:
        live = json.load(handle)
    live_stat = os.stat(rt + "/state.json")
except OSError as exc:
    die("cannot read live state: %s" % (exc.strerror or exc))
except ValueError as exc:
    die("live state.json is not valid JSON: %s" % exc)
dev_br = subprocess.run(["git", "-C", devdir, "branch", "--show-current"],
                        capture_output=True, text=True).stdout.strip()
qe_br = subprocess.run(["git", "-C", qedir, "branch", "--show-current"],
                       capture_output=True, text=True).stdout.strip()
recon = {"halt": live.get("halt", False), "issue": live.get("issue"),
         "phase": live.get("phase", ""), "state": live.get("state"),
         "retries": live.get("retries", 0),
         "retry_budget": live.get("retry_budget", 3),
         "updated": live.get("updated", ""),
         "pending": live.get("pending", "")}
out_path = copy + "/state.json"
if os.path.exists(out_path):
    st = os.stat(out_path)
    if (st.st_dev, st.st_ino) == (live_stat.st_dev, live_stat.st_ino):
        die("copy state.json is the live file (shared dev/ino)")
cst, lst = os.stat(copy), os.stat(rt)
if (cst.st_dev, cst.st_ino) == (lst.st_dev, lst.st_ino):
    die("--copy is the live runtime dir")
fd, tmp = tempfile.mkstemp(dir=copy, prefix=".recon.")
try:
    with os.fdopen(fd, "w") as handle:
        handle.write(json.dumps(recon, indent=2) + "\n")
        handle.flush()
        os.fsync(handle.fileno())
    os.replace(tmp, out_path)  # replace the entry, never truncate a shared inode
    tmp = None
finally:
    if tmp is not None:
        try:
            os.unlink(tmp)
        except OSError:
            pass
print("reconstruct: dev=%s qe=%s" % (dev_br, qe_br))
print("machine fields: " + ("MATCH" if all(
    recon[k] == live.get(k) for k in
    ("phase", "state", "retries", "retry_budget", "halt")) else "MISMATCH"))
EOF
}

# self-test only (not in usage): expose the clean-tree parser so its porcelain
# handling (renames, spaces, untracked) is testable without a live worker.
mode_check_clean_tree() {
  [ $# -ge 1 ] || fail "usage: __check-clean-tree <dir> [exempt-prefix...]"
  check_clean_tree "$@"
}

mode_self_test() {
  local t=0 pass=0
  chk() { t=$((t+1)); if "$@" >/dev/null 2>&1; then pass=$((pass+1)); echo "PASS: $*"; else echo "FAIL: $*"; fi; }
  chk_neg() { t=$((t+1)); if "$@" >/dev/null 2>&1; then echo "FAIL (should abort): $*"; else pass=$((pass+1)); echo "PASS (aborts): $*"; fi; }
  # guard aborts (scratch names only — never live sessions)
  chk_neg "$SCRIPT_DIR/agent_up.sh" run "agentup-st-$$" /nonexistent dev /dev/null || true
  # a role is a NAME: a config path is refused before any render happens
  chk_neg "$SCRIPT_DIR/agent_up.sh" run "agentup-st-$$" /tmp dev/opencode.json /dev/null || true
  # msg discipline
  chk_neg "$SCRIPT_DIR/agent_up.sh" msg "agentup-nosuch-$$" "hi" || true
  printf 'two\nlines' > /tmp/agentup-ml-$$.txt || true
  # reconstruct refuses live dir ( Triple-check: uses --copy guard)
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 410 --copy /tmp/nexios-410 || true
  rm -f /tmp/agentup-ml-$$.txt
  # issue-optional room: bare `up` opens three sessions with no runtime/briefs
  local rp
  rp=$("$SCRIPT_DIR/agent_up.sh" __room-plan)
  chk bash -c 'printf "%s" "$1" | grep -qx "issue=none"' _ "$rp"
  chk bash -c 'printf "%s" "$1" | grep -qx "dev_prompt=none"' _ "$rp"
  chk bash -c 'printf "%s" "$1" | grep -qx "sessions=nexios-dev,nexios-qe,nexios-consultant"' _ "$rp"
  # ... with an issue the briefs come from the runtime (and only then are required)
  rp=$("$SCRIPT_DIR/agent_up.sh" __room-plan 4199999)
  chk bash -c 'printf "%s" "$1" | grep -qx "dev_prompt=/tmp/nexios-4199999/prompts/dev.txt"' _ "$rp"
  chk bash -c 'printf "%s" "$1" | grep -qx "issue=4199999"' _ "$rp"
  # one canonical issue number: 04199999 addresses the same runtime
  rp=$("$SCRIPT_DIR/agent_up.sh" __room-plan 04199999)
  chk bash -c 'printf "%s" "$1" | grep -qx "issue=4199999"' _ "$rp"
  # an idea is never mistaken for an issue, and arity is capped
  chk_neg "$SCRIPT_DIR/agent_up.sh" __room-plan notanissue
  chk_neg "$SCRIPT_DIR/agent_up.sh" __room-plan 4199999 a b
  # planning a room never materialises a runtime
  chk_neg test -e /tmp/nexios-4199999
  # fence rendering: polarity, determinism, mirrors, {repo}, twins, fail-closed
  chk "$SCRIPT_DIR/agent_up.sh" render-self-test
  # ... and the verifier agrees with what the launcher renders (one source)
  chk python3 "$REPO_ROOT/tools/agents/verify_fence.py" --self-test
  # the consultant launch command carries the RESOLVED config + pinned TMPDIR
  # (S4-9: the fence assertion is only meaningful if the env is the fenced one)
  local cc; cc=$("$SCRIPT_DIR/agent_up.sh" __consult-cmd)
  chk bash -c 'printf "%s" "$1" | grep -q "OPENCODE_CONFIG=.*consultant/opencode.resolved.json"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q "TMPDIR=/tmp/agentup-tmp-"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q -- "-u OPENCODE_CONFIG_CONTENT"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q "NEXIOS_AGENT_ROLE=consultant"' _ "$cc"
  # L1/L2 renderer path + role are derived per role, never hand-passed
  local dc qc
  dc=$("$SCRIPT_DIR/agent_up.sh" __run-cmd dev)
  qc=$("$SCRIPT_DIR/agent_up.sh" __run-cmd qe)
  chk bash -c 'printf "%s" "$1" | grep -q "OPENCODE_CONFIG=.*/rendered/dev/opencode.resolved.json"' _ "$dc"
  chk bash -c 'printf "%s" "$1" | grep -q "OPENCODE_CONFIG=.*/rendered/qe/opencode.resolved.json"' _ "$qc"
  chk bash -c 'printf "%s" "$1" | grep -q "NEXIOS_AGENT_ROLE=dev"' _ "$dc"
  chk bash -c 'printf "%s" "$1" | grep -q "NEXIOS_AGENT_ROLE=qe"' _ "$qc"
  # L3 traversal / unknown roles are refused before launching
  chk_neg "$SCRIPT_DIR/agent_up.sh" __run-cmd ../../../tmp/evil
  chk_neg "$SCRIPT_DIR/agent_up.sh" __run-cmd consultant/../dev
  # a slash-free but unknown role is refused too (allowlist, not a path heuristic)
  chk_neg "$SCRIPT_DIR/agent_up.sh" __run-cmd evil
  chk_neg "$SCRIPT_DIR/agent_up.sh" run "agentup-st-$$" /tmp evil /dev/null
  # ... and the refusal happens BEFORE any filesystem side effect
  chk_neg test -e "/tmp/agentup-evil-$$"
  # L4/L5 fence show reads the same source; unknown role aborts
  chk bash -c "$SCRIPT_DIR/agent_up.sh fence show dev | grep -q '\"bash\": \"allow\"'"
  chk_neg "$SCRIPT_DIR/agent_up.sh" fence show bogus
  # issue-number canonicality: 0, over-long and leading-zero aliases
  chk_neg "$SCRIPT_DIR/agent_up.sh" setup 0
  chk_neg "$SCRIPT_DIR/agent_up.sh" setup 12345678
  # out-of-repo policy: a REAL file where a rendered config belongs hard-fails
  # (silent adoption of pre-#414 state is forbidden), legacy LINKS are pruned,
  # and the one surviving link (AGENTS.md) is versioned in-repo.
  local st="/tmp/agentup-st-$$"; mkdir -p "$st/home/coordinator" "$st/home/roles"
  printf 'drift\n' > "$st/home/coordinator/opencode.json"
  chk_neg env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9999
  rm -f "$st/home/coordinator/opencode.json"
  ln -sfn "$REPO_ROOT/tools/agents/roles/dev.json" "$st/home/roles/dev.json"
  ln -sfn /nonexistent "$st/home/coordinator/opencode.json"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9999
  chk_neg test -e "$st/home/coordinator/AGENTS.md"
  chk_neg test -e "$st/home/coordinator/opencode.json"
  chk_neg test -e "$st/home/roles/dev.json"
  # leading-zero issue numbers canonicalize onto ONE runtime (never alias 410)
  local zcan
  local zraw
  zcan=$((900000 + ($$ % 90000)))
  zraw="000$zcan"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup "$zraw"
  chk test -f "/tmp/nexios-$zcan/state.json"
  chk_neg test -e "/tmp/nexios-$zraw"
  # every role is rendered by setup; consultant {repo} keys are expanded
  local rr
  for rr in dev qe auditor consultant; do
    chk test -f "$st/home/rendered/$rr/opencode.resolved.json"
  done
  chk_neg grep -q '{repo}' "$st/home/rendered/consultant/opencode.resolved.json"
  chk python3 -c "import json,sys; json.load(open(sys.argv[1]))" \
      "$st/home/rendered/consultant/opencode.resolved.json"
  # state writer: validated keys/vocabulary, locked, atomic
  chk_neg "$SCRIPT_DIR/agent_up.sh" state 9999 bogus=1
  chk_neg "$SCRIPT_DIR/agent_up.sh" state 9999 state=NONSENSE
  chk_neg "$SCRIPT_DIR/agent_up.sh" state 9999 retries=abc
  chk "$SCRIPT_DIR/agent_up.sh" state 9999 phase=AUDITED retries=1
  chk "$SCRIPT_DIR/agent_up.sh" state 9999 release_armed=true
  chk grep -q '"release_armed": true' /tmp/nexios-9999/state.json
  chk "$SCRIPT_DIR/agent_up.sh" state 9999 gh_notes=true
  chk grep -q '"gh_notes": true' /tmp/nexios-9999/state.json
  chk grep -q '"phase": "AUDITED"' /tmp/nexios-9999/state.json
  chk grep -q '"retries": 1' /tmp/nexios-9999/state.json
  # ... and a corrupt state file is a one-line error, never a traceback
  local bad="/tmp/nexios-9991"; rm -rf "$bad"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9991
  printf '{not json' > "$bad/state.json"
  local jout; jout=$("$SCRIPT_DIR/agent_up.sh" state 9991 phase=PLANNED 2>&1 || true)
  chk bash -c 'printf "%s" "$1" | grep -q "state: existing state.json is not valid JSON"' _ "$jout"
  chk_neg bash -c 'printf "%s" "$1" | grep -q Traceback' _ "$jout"
  # --copy anchor: nested path and wrong suffix must be refused
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 410 --copy /tmp/a/b-copy
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 410 --copy /tmp/plain
  # --copy is a symlink / shares the live inode -> refused, live state unchanged
  local sl="/tmp/agentup-sl-$$-copy"
  rm -rf "$sl"; ln -s /tmp "$sl"
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 9999 --copy "$sl"
  rm -f "$sl"
  local hl="/tmp/agentup-hl-$$-copy"
  rm -rf "$hl"; mkdir -p "$hl"
  ln -s /tmp/nexios-9999/state.json "$hl/state.json"
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 9999 --copy "$hl"
  rm -rf "$hl"; mkdir -p "$hl"
  ln /tmp/nexios-9999/state.json "$hl/state.json"
  local before after
  before=$(shasum /tmp/nexios-9999/state.json | cut -d' ' -f1)
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 9999 --copy "$hl"
  after=$(shasum /tmp/nexios-9999/state.json | cut -d' ' -f1)
  chk test "$before" = "$after"
  rm -rf "$hl"
  # clean-tree parser: renames report the NEW path, spaces survive, untracked is
  # in scope, exempt prefixes stay exempt
  local ct="$st/clean"; mkdir -p "$ct/graphify-out"
  git -C "$ct" init -q -b main
  printf 'a\n' > "$ct/a b.txt"
  git -C "$ct" add -A
  git -C "$ct" -c user.email=t@t -c user.name=t commit -qm base
  git -C "$ct" mv "a b.txt" "x y.txt"
  printf 'z\n' > "$ct/z.txt"
  printf 'q\n' > "$ct/graphify-out/x"
  local ctout; ctout=$("$SCRIPT_DIR/agent_up.sh" __check-clean-tree "$ct" graphify-out/ 2>&1 || true)
  chk bash -c 'printf "%s" "$1" | grep -q "x y.txt"' _ "$ctout"
  chk bash -c 'printf "%s" "$1" | grep -q "z.txt"' _ "$ctout"
  chk_neg bash -c 'printf "%s" "$1" | grep -q "a b.txt"' _ "$ctout"
  chk_neg bash -c 'printf "%s" "$1" | grep -q "graphify-out/x"' _ "$ctout"
  git -C "$ct" add -A
  git -C "$ct" -c user.email=t@t -c user.name=t commit -qm clean
  chk "$SCRIPT_DIR/agent_up.sh" __check-clean-tree "$ct" graphify-out/
  # audit-diff: explicit base, temp-file write (no self-deletion hunk), exclusions
  local gd="$st/gd"; mkdir -p "$gd/audits"
  git -C "$gd" init -q -b main
  printf 'base\n' > "$gd/a.txt"
  printf 'old\n' > "$gd/audits/pending_patch.diff"
  git -C "$gd" add -A
  git -C "$gd" -c user.email=t@t -c user.name=t commit -qm base
  printf 'change\n' >> "$gd/a.txt"
  chk bash -c "cd '$gd' && '$SCRIPT_DIR/agent_up.sh' audit-diff main"
  chk grep -q '^+change' "$gd/audits/pending_patch.diff"
  chk_neg grep -q 'pending_patch.diff' "$gd/audits/pending_patch.diff"
  chk bash -c "cd '$gd' && '$SCRIPT_DIR/agent_up.sh' audit-diff main --base main"
  chk_neg bash -c "cd '$gd' && '$SCRIPT_DIR/agent_up.sh' audit-diff main --base="
  # clean: arity + numeric issues (no repo contact on these paths)
  chk_neg "$SCRIPT_DIR/agent_up.sh" clean
  chk_neg "$SCRIPT_DIR/agent_up.sh" clean notanissue
  chk_neg "$SCRIPT_DIR/agent_up.sh" clean 0
  chk_neg "$SCRIPT_DIR/agent_up.sh" clean 1 2 3
  chk_neg "$SCRIPT_DIR/agent_up.sh" clean 9999 --bogus
  chk_neg "$SCRIPT_DIR/agent_up.sh" clean 00
  # clean predicate guard rails (protected wins by check order)
  chk bash -c 'test "$("$0" __clean-pred fix/422-x)" = owned' "$SCRIPT_DIR/agent_up.sh"
  chk bash -c 'test "$("$0" __clean-pred fence/gap9)" = owned' "$SCRIPT_DIR/agent_up.sh"
  chk bash -c 'test "$("$0" __clean-pred main)" = protected' "$SCRIPT_DIR/agent_up.sh"
  chk bash -c 'test "$("$0" __clean-pred testbed)" = protected' "$SCRIPT_DIR/agent_up.sh"
  chk bash -c 'test "$("$0" __clean-pred feature/x)" = foreign' "$SCRIPT_DIR/agent_up.sh"
  # clean fixtures: gh seam (OPEN vs CLOSED) + scratch repo with fake origin
  local ghf="$st/gh"; mkdir -p "$ghf"
  printf '#!/bin/sh\nprintf "OPEN\\n"\n' > "$ghf/gh-open"; chmod +x "$ghf/gh-open"
  printf '#!/bin/sh\nprintf "CLOSED\\n"\n' > "$ghf/gh-closed"; chmod +x "$ghf/gh-closed"
  # argv recorder: proves clean resolves --repo explicitly (item 2, #427).
  # Records gh's argv to $GH_ARGV_FILE, then answers CLOSED like gh-closed.
  printf '#!/bin/sh\necho "$@" > "$GH_ARGV_FILE"\nprintf "CLOSED\\n"\n' > "$ghf/gh-record"; chmod +x "$ghf/gh-record"
  local cr="$st/clean"; mkdir -p "$cr"
  git init -q --bare -b main "$cr/origin.git"
  git init -q -b main "$cr/repo"
  git -C "$cr/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m base
  git -C "$cr/repo" remote add origin "$cr/origin.git"
  git -C "$cr/repo" push -q origin main
  # refusal: open issue aborts before any repo contact
  chk_neg env NEXIOS_GH_BIN="$ghf/gh-open" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run
  # refusal: unpushed main aborts (gh passes, push check fails)
  git -C "$cr/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m ahead
  chk_neg env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run
  git -C "$cr/repo" push -q origin main
  # refusal: unknown __clean-repo flag is fail-closed, never silently live
  chk_neg env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --bogus
  # dry-run listing: merged owned WOULD REMOVE, unmerged KEPT, nothing moves
  git -C "$cr/repo" checkout -q -b fix/st-merged
  git -C "$cr/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m lane
  git -C "$cr/repo" checkout -q main
  git -C "$cr/repo" -c user.email=t@t -c user.name=t merge -q --no-ff -m merge-st-merged fix/st-merged
  git -C "$cr/repo" push -q origin main
  git -C "$cr/repo" checkout -q -b fix/st-open
  git -C "$cr/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m wip
  git -C "$cr/repo" checkout -q main
  local clout
  clout=$(env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run || true)
  chk bash -c 'printf "%s" "$1" | grep -q "WOULD REMOVE branch: fix/st-merged"' _ "$clout"
  chk bash -c 'printf "%s" "$1" | grep -q "KEPT (not merged): fix/st-open"' _ "$clout"
  chk bash -c 'printf "%s" "$1" | grep -q "KEPT (protected): main"' _ "$clout"
  chk git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-merged
  # live run deletes only the merged owned branch (room absent path)
  env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 >/dev/null
  chk_neg git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-merged
  chk git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-open
  # clean from a feature-branch checkout: HEAD-coupling must not block
  # deletion of main-merged lanes (#425). Same repo, new lane merged
  # into main while parked on an unrelated feature branch.
  git -C "$cr/repo" checkout -q -b fix/st-feature
  git -C "$cr/repo" checkout -q -b fix/st-alien main
  git -C "$cr/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m lane2
  git -C "$cr/repo" checkout -q main
  git -C "$cr/repo" -c user.email=t@t -c user.name=t merge -q --no-ff -m merge-st-alien fix/st-alien
  git -C "$cr/repo" push -q origin main
  git -C "$cr/repo" checkout -q fix/st-feature
  local drylist livelist
  drylist=$(env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run)
  chk bash -c 'printf "%s" "$1" | grep -q "WOULD REMOVE branch: fix/st-alien"' _ "$drylist"
  chk bash -c 'printf "%s" "$1" | grep -q "KEPT (checked out): fix/st-feature"' _ "$drylist"
  livelist=$(env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996)
  chk bash -c 'printf "%s" "$1" | grep -q "REMOVED branch: fix/st-alien"' _ "$livelist"
  chk_neg git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-alien
  chk git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-open
  chk git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-feature
  # dry-run listing matches the real run branch-for-branch (#425 parity)
  chk bash -c 'test "$(printf "%s" "$1" | grep -o "WOULD REMOVE branch: .*" | awk "{print \$NF}" | sort)" = "$(printf "%s" "$2" | grep -o "REMOVED branch: .*" | awk "{print \$NF}" | sort)"' _ "$drylist" "$livelist"
  # dry-run mirrors the live worktree guard (QE addendum: WOULD REMOVE here
  # must never become a live refusal). Merged owned branch checked out in
  # a second worktree: dry-run KEPT, live refuses, nothing moves.
  git -C "$cr/repo" checkout -q -b fix/st-linked main
  git -C "$cr/repo" -c user.email=t@t -c user.name=t commit -q --allow-empty -m lane3
  git -C "$cr/repo" checkout -q main
  git -C "$cr/repo" -c user.email=t@t -c user.name=t merge -q --no-ff -m merge-st-linked fix/st-linked
  git -C "$cr/repo" push -q origin main
  git -C "$cr/repo" checkout -q fix/st-feature
  git -C "$cr/repo" worktree add "$st/wt2" fix/st-linked >/dev/null
  local drywt
  drywt=$(env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run)
  chk bash -c 'printf "%s" "$1" | grep -q "KEPT (checked out in another worktree): fix/st-linked"' _ "$drywt"
  # dry==real on a worktree-linked branch: the live run succeeds without
  # touching it (both paths share the guard, so live can neither delete
  # nor refuse where dry listed KEPT).
  chk env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996
  chk git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-linked
  chk test -d "$st/wt2"
  git -C "$cr/repo" worktree remove --force "$st/wt2"
  git -C "$cr/repo" worktree prune
  # item 2 (#427): gh gets an explicit --repo derived from origin, even
  # with a cwd outside any repo. github-style origin first.
  git -C "$cr/repo" remote set-url origin "git@github.com:acme/widget.git"
  (cd / && env NEXIOS_GH_BIN="$ghf/gh-record" GH_ARGV_FILE="$st/gh-argv" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run >/dev/null)
  chk grep -q -- "--repo acme/widget" "$st/gh-argv"
  # unparseable (local-path) origin degrades to ambient gh resolution.
  git -C "$cr/repo" remote set-url origin "$cr/origin.git"
  (cd / && env NEXIOS_GH_BIN="$ghf/gh-record" GH_ARGV_FILE="$st/gh-argv" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run >/dev/null)
  chk_neg grep -q -- "--repo" "$st/gh-argv"
  # item 3 (#427): owned+merged branch sitting exactly at the main tip
  # (empty lane, nothing committed) is KEPT with an explicit reason, in
  # dry-run and live alike. Parked on an unrelated branch.
  git -C "$cr/repo" checkout -q -b fix/st-empty main
  git -C "$cr/repo" checkout -q fix/st-feature
  local dryempty
  dryempty=$(env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996 --dry-run)
  chk bash -c 'printf "%s" "$1" | grep -q "KEPT (at main tip.*): fix/st-empty"' _ "$dryempty"
  chk env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9996
  chk git -C "$cr/repo" show-ref --verify -q refs/heads/fix/st-empty
  # gate.py self-test covers the check-feedback main-vs-worktree fix
  chk python3 "$REPO_ROOT/scripts/gate.py" self-test
  # live room removal requires closed-state: PLANNED room refuses first
  local r97="/tmp/nexios-9997"; rm -rf "$r97"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9997
  chk_neg env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9997
  chk test -d "$r97"
  "$SCRIPT_DIR/agent_up.sh" state 9997 phase=CLOSED >/dev/null
  mkdir -p /tmp/nexios-9997-backupX
  env NEXIOS_GH_BIN="$ghf/gh-closed" \
      "$SCRIPT_DIR/agent_up.sh" __clean-repo "$cr/repo" 9997 >/dev/null
  chk_neg test -e "$r97"
  chk_neg test -e /tmp/nexios-9997-backupX
  # watchdog: verbs refuse bad input; tick on missing room fails loud
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch notanissue
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch 9999 bogus
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch 0 tick
  rm -rf /tmp/nexios-9990
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch 9990 tick
  # watchdog: tick state machine is fixture-driven (live workers make real
  # fingerprints nondeterministic); plus one live smoke test proving the
  # real probes run end-to-end.
  local r93="/tmp/nexios-9993"; rm -rf "$r93"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9993
  chk bash -c '"$0" watch 9993 tick | grep -qE "^(BASELINE|UNCHANGED|CHANGED) 9993"' "$SCRIPT_DIR/agent_up.sh"
  rm -f "$r93"/watch.fingerprint "$r93"/watch.now "$r93"/watch.unchanged
  local fpf="$st/fp"; printf 'v1\n' > "$fpf"
  chk bash -c 'NEXIOS_WATCHDOG_FAKE_FP="$1" "$0" watch 9993 tick | grep -q "^BASELINE 9993$"' "$SCRIPT_DIR/agent_up.sh" "$fpf"
  chk bash -c 'NEXIOS_WATCHDOG_FAKE_FP="$1" "$0" watch 9993 tick | grep -q "^UNCHANGED 9993 x1$"' "$SCRIPT_DIR/agent_up.sh" "$fpf"
  chk bash -c 'NEXIOS_WATCHDOG_FAKE_FP="$1" "$0" watch 9993 tick | grep -q "^UNCHANGED 9993 x2$"' "$SCRIPT_DIR/agent_up.sh" "$fpf"
  chk bash -c 'NEXIOS_WATCHDOG_FAKE_FP="$1" "$0" watch 9993 tick | grep -q "^UNCHANGED 9993 x3 ESCALATE$"' "$SCRIPT_DIR/agent_up.sh" "$fpf"
  printf 'v2\n' > "$fpf"
  chk bash -c 'NEXIOS_WATCHDOG_FAKE_FP="$1" "$0" watch 9993 tick | grep -q "^CHANGED 9993 "' "$SCRIPT_DIR/agent_up.sh" "$fpf"
  chk bash -c 'NEXIOS_WATCHDOG_FAKE_FP="$1" "$0" watch 9993 tick | grep -q "^UNCHANGED 9993 x1$"' "$SCRIPT_DIR/agent_up.sh" "$fpf"
  # watchdog: arm/disarm pid lifecycle, double-arm refusal, status
  "$SCRIPT_DIR/agent_up.sh" watch 9993 arm 3600 >/dev/null
  chk test -f "$r93/watch.pid"
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch 9993 arm 3600
  chk bash -c '"$0" watch 9993 status | grep -q "^armed pid=[0-9]* streak=[0-9]*$"' "$SCRIPT_DIR/agent_up.sh"
  "$SCRIPT_DIR/agent_up.sh" watch 9993 disarm >/dev/null
  chk_neg test -e "$r93/watch.pid"
  chk bash -c '"$0" watch 9993 status | grep -q "^disarmed streak=[0-9]*$"' "$SCRIPT_DIR/agent_up.sh"
  chk_neg "$SCRIPT_DIR/agent_up.sh" watch 9993 disarm
  rm -rf "$r93"
  rm -rf "$st" "/tmp/nexios-$zcan" "/tmp/nexios-9991" "/tmp/nexios-9999" \
         "$REPO_ROOT/.nexios-runtime/$zcan" "$REPO_ROOT/.nexios-runtime/9991" \
         "$REPO_ROOT/.nexios-runtime/9999"
  echo "self-test: $pass/$t PASS"
  [ "$pass" = "$t" ] || fail "self-test failures"
}

# $1 role — print the composed map. Reads the same source the launcher uses, so
# "what will the agent get" is answerable without launching anything.
mode_fence_show() {
  case "${1:-}" in
    show)
      [ $# -eq 2 ] || fail "fence show needs a role (dev|qe|auditor|consultant)"
      need_cmd python3
      need_file "$RENDERER"
      python3 "$RENDERER" --role "$2" --print --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR"
      ;;
    install)
      need_cmd python3
      need_file "$RENDERER"
      local dir="${2:-$REPO_ROOT}"
      render_all >/dev/null
      install_agents "$dir"
      info "agents installed in $dir/.opencode/agents"
      ;;
    *) fail "usage: fence show <role> | fence install [dir]" ;;
  esac
}

# $1 role — print the exact command run_session sends (self-test + inspection).
# self-test only: resolve what `up` would do, without tmux/opencode/render.
mode_room_plan() {
  [ $# -le 2 ] || fail "up takes at most: up [issue] [taskfile-or-task] (got $#)"
  local issue="" task="" dp="" qp=""
  if [ $# -ge 1 ]; then
    local rt; rt=$(runtime_dir "$1")
    issue="${rt##*/nexios-}"
    if [ $# -eq 2 ]; then
      if [ -f "$2" ]; then task=file; else task=text; fi
    fi
    dp="$rt/prompts/$(role_field dev prompt)"
    qp="$rt/prompts/$(role_field qe prompt)"
  fi
  printf 'issue=%s\ntask=%s\ndev_prompt=%s\nqe_prompt=%s\nsessions=%s,%s,%s\n' \
    "${issue:-none}" "${task:-none}" "${dp:-none}" "${qp:-none}" \
    "$DEV_SESS" "$QE_SESS" "$CONSULT_SESS"
}

mode_launch_cmd() {
  [ $# -eq 1 ] || fail "__run-cmd needs a role"
  case "$1" in dev|qe|auditor|consultant) ;; *) fail "__run-cmd needs a role (dev|qe|auditor|consultant), got '$1'" ;; esac
  local cfg
  cfg=$(render_role_config "$1")
  printf 'env -u OPENCODE_CONFIG -u OPENCODE_CONFIG_CONTENT TMPDIR=/tmp/agentup-tmp-agentup-probe OPENCODE_CONFIG=%s NEXIOS_AGENT_ROLE=%s %s\n' \
    "$cfg" "$1" "$OPENCODE_BIN"
}

# Renderer/verifier matrix. No tmux, no opencode, no $NEXIOS_HOME: everything
# lands in a scratch dir, so this is safe in CI and in a live session.
mode_render_self_test() {
  local t=0 pass=0
  chk() { t=$((t+1)); if "$@" >/dev/null 2>&1; then pass=$((pass+1)); echo "PASS: $*"; else echo "FAIL: $*"; fi; }
  chk_neg() { t=$((t+1)); if "$@" >/dev/null 2>&1; then echo "FAIL (should abort): $*"; else pass=$((pass+1)); echo "PASS (aborts): $*"; fi; }
  local st
  st=$(mktemp -d /tmp/agentup-render-XXXXXX)
  local out="$st/out" tw="$st/twins"
  mkdir -p "$out" "$tw"

  cfg_val() {  # $1 file, $2 section, $3 key
    python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["permission"][sys.argv[2]].get(sys.argv[3],""))' "$@"
  }
  mirrors_ok() {
    python3 - "$1" <<'PY'
import json, sys
p = json.load(open(sys.argv[1]))["permission"]
for sect in ("external_directory", "edit"):
    m = p[sect]
    for k in list(m):
        if k.startswith("/tmp/"):
            assert m.get("/private" + k) == m[k], (sect, k)
        elif k.startswith("/private/tmp/"):
            assert m.get(k[len("/private"):]) == m[k], (sect, k)
PY
  }
  no_host_tmp() { ! grep -q '/var/folders' "$1"; }
  consultant_ok() {
    python3 - "$1" "$REPO_ROOT" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
repo = sys.argv[2]
assert "mcp" in cfg and "github" in cfg["mcp"], "mcp lost"
edit = cfg["permission"]["edit"]
ext = cfg["permission"]["external_directory"]
allows = {k for k, v in edit.items() if v == "allow"}
assert allows == {"/tmp/nexios-*/*", "/private/tmp/nexios-*/*",
                  "/tmp/nexios-*/**/*", "/private/tmp/nexios-*/**/*",
                  repo + "/logs/**"}, allows
repos = {k: v for k, v in list(edit.items()) + list(ext.items()) if k.startswith(repo)}
assert set(repos) == {repo + "/prompts/**", repo + "/audits/**",
                      repo + "/scripts/**", repo + "/tools/agents/**",
                      repo + "/logs/**"}, repos
assert all(v == "allow" for v in repos.values())
PY
  }
  worker_ok() {  # $1 file, $2 repo
    python3 - "$1" "$2" <<'PY'
import json, sys
text = open(sys.argv[1]).read()
assert "{repo}" not in text, "template token leaked"
assert sys.argv[2] + "/prompts" not in text, "worker carries a repo path"
PY
  }
  twin_ok() {  # $1 twin file, $2 rendered config
    python3 - "$1" "$2" <<'PY'
import json, sys
lines = open(sys.argv[1]).read().splitlines()
cfg = json.load(open(sys.argv[2]))["permission"]
assert lines[0] == "---" and "---" in lines[1:], "twin frontmatter missing"
maps, sect = {"external_directory": {}, "edit": {}}, None
for line in lines[1:]:
    if line == "---":
        break
    if line == "  external_directory:":
        sect = "external_directory"
    elif line == "  edit:":
        sect = "edit"
    elif line.startswith("    ") and sect:
        k, _, v = line.strip().partition(": ")
        maps[sect][json.loads(k)] = v
assert maps["external_directory"] == cfg["external_directory"], "external drift"
assert maps["edit"] == cfg["edit"], "edit drift"
PY
  }
  fixture() {  # $1 name -> fence dir with an independent copy of the sources
    local d="$st/fx/$1"
    mkdir -p "$d/roles"
    cp "$FENCE_DIR/base.json" "$d/base.json"
    cp "$FENCE_DIR"/roles/*.json "$d/roles/"
    printf '%s\n' "$d"
  }
  mutate() {  # $1 json file, $2 python statements over `d`
    python3 - "$1" "$2" <<'PY'
import json, sys
path, code = sys.argv[1], sys.argv[2]
d = json.load(open(path))
exec(code)
with open(path, "w") as h:
    json.dump(d, h, indent=2)
PY
  }
  render() {
    local role="$1" out="$2"; shift 2
    python3 "$RENDERER" --role "$role" --out "$out" --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR" "$@"
  }

  # R1-R4 role polarity + validity
  local r f
  for r in dev qe auditor consultant; do
    f="$out/$r/opencode.resolved.json"
    chk render "$r" "$f" --twins-out "$out/$r"
    chk test -s "$f"
    case "$r" in
      dev) chk test "$(cfg_val "$f" edit '*')" = allow ;;
      *)   chk test "$(cfg_val "$f" edit '*')" = deny ;;
    esac
  done
  # R5 determinism, R6 role isolation
  chk render dev "$st/again/opencode.resolved.json"
  chk cmp -s "$out/dev/opencode.resolved.json" "$st/again/opencode.resolved.json"
  local hashes
  if command -v sha256sum >/dev/null 2>&1; then
    hashes=$(sha256sum "$out"/*/opencode.resolved.json | awk '{print $1}' | sort -u | wc -l | tr -d ' ')
  else
    hashes=$(shasum -a 256 "$out"/*/opencode.resolved.json | awk '{print $1}' | sort -u | wc -l | tr -d ' ')
  fi
  chk test "$hashes" = 4
  # R7 mirrors, R8 host TMPDIR, R9 consultant keys, R10 worker purity
  for r in dev qe auditor consultant; do
    chk mirrors_ok "$out/$r/opencode.resolved.json"
    chk no_host_tmp "$out/$r/opencode.resolved.json"
  done
  chk consultant_ok "$out/consultant/opencode.resolved.json"
  for r in dev qe auditor; do
    chk worker_ok "$out/$r/opencode.resolved.json" "$REPO_ROOT"
  done
  # R11 generated twins match their rendered map
  chk twin_ok "$out/dev/nexios-dev.md" "$out/dev/opencode.resolved.json"
  chk twin_ok "$out/qe/nexios-qe.md" "$out/qe/opencode.resolved.json"
  chk twin_ok "$out/auditor/nexios-audit.md" "$out/auditor/opencode.resolved.json"
  chk twin_ok "$out/consultant/nexios-consultant.md" "$out/consultant/opencode.resolved.json"

  # N1-N13 fail-closed probes
  chk_neg python3 "$RENDERER" --role bogus --print --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR"
  local d
  d=$(fixture n2); rm -f "$d/roles/qe.json"
  chk_neg python3 "$RENDERER" --role qe --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n3); mutate "$d/roles/dev.json" 'd["edit_deny"].append("{repo}/src/x")'
  chk_neg python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n4); mutate "$d/roles/consultant.json" 'd["edit_allow"].append("prompts/LEARNINGS.md")'
  chk_neg python3 "$RENDERER" --role consultant --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n5); mutate "$d/roles/dev.json" 'd["external_allow"].append("/tmp/nexios-*")'
  chk_neg python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n6); mutate "$d/roles/qe.json" 'd["edit_deny"].append("/tmp/jarvis-*")'
  chk_neg python3 "$RENDERER" --role qe --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n7); mutate "$d/roles/qe.json" 'd["role"] = "dev"'
  chk_neg python3 "$RENDERER" --role qe --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n8); mutate "$d/roles/dev.json" 'd["edit_deny_extra"] = []'
  chk_neg python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$d"
  chk_neg python3 "$RENDERER" --role dev --out "$REPO_ROOT/.render-probe.json" --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR"
  printf 'x\n' > "$st/regular"
  chk_neg python3 "$RENDERER" --role dev --out "$st/regular/x.json" --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR"
  d=$(fixture n11); mutate "$d/base.json" 'd["namespaces"].append({"root": "/private/tmp/x", "decision": "allow"})'
  chk_neg python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n12); mutate "$d/roles/qe.json" 'd.pop("edit_default")'
  chk_neg python3 "$RENDERER" --role qe --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n13); mutate "$d/roles/auditor.json" 'd["edit_allow"].append("*")'
  chk_neg python3 "$RENDERER" --role auditor --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n14); mutate "$d/roles/dev.json" 'd["edit_allow"].append("/etc/cron.d/x")'
  chk_neg python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n15); mutate "$d/roles/qe.json" 'd["external_allow"].append("/private/tmp/agentup-*")'
  chk_neg python3 "$RENDERER" --role qe --print --repo-root "$REPO_ROOT" --fence-root "$d"
  # role-scoped namespaces: the auditor's /tmp scratch must not leak to workers
  d=$(fixture n16); mutate "$d/base.json" 'd["namespaces"].append({"root": "/tmp/*", "decision": "allow", "roles": ["nobody"], "external_variants": [""]})'
  chk_neg python3 "$RENDERER" --role auditor --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n17); mutate "$d/base.json" 'd["namespaces"].append({"root": "/tmp/*", "decision": "allow", "roles": []})'
  chk_neg python3 "$RENDERER" --role auditor --print --repo-root "$REPO_ROOT" --fence-root "$d"

  # auditor-only /tmp scratch: present for the auditor, absent for every worker
  local au dv
  au=$(python3 "$RENDERER" --role auditor --print --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR")
  dv=$(python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR")
  chk bash -c 'printf "%s" "$1" | grep -q "\"/tmp/\*\": \"allow\""' _ "$au"
  chk_neg bash -c 'printf "%s" "$1" | grep -q "\"/tmp/\*\": \"allow\""' _ "$dv"

  rm -rf "$st"
  echo "render-self-test: $pass/$t PASS"
  [ "$pass" = "$t" ] || fail "render-self-test failures"
}

mode_approve() {
  [ $# -eq 1 ] || fail "approve needs a tmux session name"
  tmux has-session -t "$1" 2>/dev/null || fail "no such tmux session: $1"
  local dlg
  dlg=$(tmux capture-pane -t "$1" -p | grep -B1 -A6 "Permission required" | head -12 || true)
  if [ -z "$dlg" ]; then
    fail "no permission dialog open on '$1' — not touching input (refusing blind keypresses)"
  fi
  echo "--- dialog on '$1' (allowing ALWAYS) ---"
  echo "$dlg"
  echo "--- driving Allow always + Confirm ---"
  tmux send-keys -t "$1" Right && sleep 1
  tmux send-keys -t "$1" Enter && sleep 6
  if tmux capture-pane -t "$1" -p | grep -q "Confirm"; then
    tmux send-keys -t "$1" Enter && sleep 5
  fi
  tmux capture-pane -t "$1" -p | tail -5
}

# Lane cleanup (operator-ordered automation): remove a CLOSED lane's merged
# branches + runtime room, guarded. Order is fail-safe: every refusal fires
# BEFORE any deletion. NEVER pushes, NEVER fetches, NEVER force-deletes.
# gh binary is overridable for self-test fixtures (NEXIOS_GH_BIN, default
# gh) — same seam pattern as TEST_TIMEOUT_OVERRIDE.
clean_issue_num() { # $1 raw -> canonical decimal, fail-loud
  case "$1" in ''|*[!0-9]*) fail "clean: issue must be numeric: '$1'";; esac
  [ "$1" != "0" ] || fail "clean: issue must be numeric: '$1'"
  local n; n=$((10#$1))
  [ "$n" != "0" ] || fail "clean: issue must be numeric: '$1'"
  printf '%s' "$n"
}
clean_gh_repo() { # $1 repo dir -> owner/repo or "" (read-only, never fails)
  # Item 2 (#427): resolve the GitHub repo explicitly from the resolved
  # repo root's origin, so `clean` works from any cwd with no GH_REPO
  # requirement. Unparseable origins (local-path fixtures) yield "" and
  # the caller falls back to ambient gh resolution.
  local url rest owner name
  url=$(git -C "$1" remote get-url origin 2>/dev/null) || return 0
  case "$url" in
    git@github.com:*) rest=${url#git@github.com:} ;;
    ssh://git@github.com/*) rest=${url#ssh://git@github.com/} ;;
    https://github.com/*) rest=${url#https://github.com/} ;;
    http://github.com/*) rest=${url#http://github.com/} ;;
    *) return 0 ;;
  esac
  rest=${rest%.git}
  case "$rest" in */*) ;; *) return 0 ;; esac
  owner=${rest%%/*}; name=${rest#*/}
  case "$owner" in ""|*/*) return 0 ;; esac
  case "$name" in ""|*/*) return 0 ;; esac
  printf '%s/%s' "$owner" "$name"
}
clean_gh_state() { # $1 repo dir, $2 canonical issue -> state word, fail-loud
  local st rr
  rr=$(clean_gh_repo "$1")
  if [ -n "$rr" ]; then
    st=$("${NEXIOS_GH_BIN:-gh}" issue view "$2" --repo "$rr" --json state --jq .state 2>/dev/null) \
      || fail "clean: cannot read issue state (gh failed for #$2)"
  else
    st=$("${NEXIOS_GH_BIN:-gh}" issue view "$2" --json state --jq .state 2>/dev/null) \
      || fail "clean: cannot read issue state (gh failed for #$2)"
  fi
  [ -n "$st" ] || fail "clean: empty issue state for #$2"
  printf '%s' "$st"
}
is_owned_branch() { # $1 short branch name (lane patterns only)
  case "$1" in
    fix/*|fence/*|comms/*|build/*|cutover/*|doctrine/*) return 0;;
    *) return 1;;
  esac
}
is_protected_branch() { # $1 short branch name (never touched, even if merged)
  case "$1" in
    main|testbed|arm-dev|risc-dev) return 0;;
    *) return 1;;
  esac
}
# $1 repo dir, $2 canonical issue, $3 [--dry-run]. Reports REMOVED:/KEPT:/
# WOULD REMOVE: lines; exit nonzero only on refusal.
__clean_repo() {
  local repo="$1" issue="$2" dry=0
  [ $# -le 3 ] || fail "clean: too many arguments"
  case "${3:-}" in ''|--dry-run) ;; *) fail "clean: unknown flag: '${3:-}'";; esac
  [ "${3:-}" = "--dry-run" ] && dry=1
  [ -d "$repo/.git" ] || fail "clean: not a git repo: $repo"
  # 1. issue CLOSED on GitHub (verify only).
  local st; st=$(clean_gh_state "$repo" "$issue")
  [ "$st" = "CLOSED" ] || fail "clean: issue #$issue is $st (must be CLOSED)"
  # 2. main == origin/main (verify only — never push, never fetch).
  local mt ot
  mt=$(git -C "$repo" rev-parse main 2>/dev/null) \
    || fail "clean: no local main in $repo"
  ot=$(git -C "$repo" rev-parse origin/main 2>/dev/null) \
    || fail "clean: no origin/main in $repo"
  [ "$mt" = "$ot" ] \
    || fail "clean: main ($mt) != origin/main ($ot) — push first (never auto-pushed)"
  # 3. room closed-state (when present; absent room is nothing to do).
  local room="/tmp/nexios-$issue" rstate=""
  if [ -d "$room" ]; then
    rstate=$(python3 -c "import json,sys; print(json.load(open(sys.argv[1])).get('phase',''))" \
      "$room/state.json" 2>/dev/null) \
      || fail "clean: room $room state unreadable (must be closed-state)"
    [ "$rstate" = "CLOSED" ] \
      || fail "clean: room $room phase is $rstate (must be CLOSED)"
  fi
  # 4. branches: protected first (disjoint from owned by construction, but
  # the check order makes that a verified property, not an assumption).
  # Merged-ness is tested against refs/heads/main explicitly — never the
  # ambient HEAD — so cleanup works from any checkout (#425). Deletion is
  # a gated `update-ref -d` (never -d/-D): `git branch -d` re-tests
  # ancestry against HEAD and would refuse a main-merged lane on a feature
  # checkout, while -D skips the check entirely. The gate below IS the
  # check; dry-run and live share this exact path so they cannot disagree.
  local b block_reason
  for b in $(git -C "$repo" for-each-ref --format='%(refname:short)' refs/heads/); do
    if is_protected_branch "$b"; then info "KEPT (protected): $b"; continue; fi
    if ! is_owned_branch "$b"; then info "KEPT (foreign): $b"; continue; fi
    if ! is_merged_into_main "$repo" "$b"; then
      info "KEPT (not merged): $b"; continue
    fi
    # Live-tree guards run on the listing path too, through the same
    # predicate the live delete uses — dry-run and real runs cannot
    # disagree on checked-out branches (#425 addendum).
    block_reason=$(clean_delete_guard "$repo" "$b")
    if [ -n "$block_reason" ]; then
      info "KEPT ($block_reason): $b"; continue
    fi
    # Item 3 (#427): an owned+merged branch sitting exactly at the main
    # tip is an empty lane — nothing committed — whose commit may be in
    # preparation. Keep with an explicit reason, never list for removal.
    # (Lane closes land as merge commits, so a tip-equal branch contributed
    # nothing; a hand-ff'd lane also lands here — kept, fail-safe toward
    # keeping. Checked-out/worktree branches returned above with the more
    # actionable reason.)
    if [ "$(git -C "$repo" rev-parse --verify "$b" 2>/dev/null)" = "$mt" ]; then
      info "KEPT (at main tip - empty lane, may be in preparation): $b"; continue
    fi
    if [ "$dry" = 1 ]; then info "WOULD REMOVE branch: $b"; continue; fi
    clean_delete_branch "$repo" "$b"
    info "REMOVED branch: $b"
  done
  # 5. room + backups (live mode only).
  if [ -d "$room" ]; then
    if [ "$dry" = 1 ]; then
      info "WOULD REMOVE room: $room"
    else
      rm -rf "$room"
      local p; for p in /tmp/nexios-"$issue"-backup*; do
        [ -e "$p" ] || continue
        rm -rf "$p"; info "REMOVED backup: $p"
      done
      info "REMOVED room: $room"
    fi
  else
    info "KEPT (room already absent): $room"
  fi
}
is_merged_into_main() { # $1 repo $2 branch — explicit main ref, never HEAD
  git -C "$1" merge-base --is-ancestor "$2" refs/heads/main 2>/dev/null
}
clean_delete_guard() { # $1 repo $2 branch — "" if deletable, else KEPT reason
  # Single source of truth for BOTH the dry-run listing and the live
  # delete: a guard that lives in only one path lets dry-run and reality
  # disagree (the #425 addendum class of bug — WOULD REMOVE here, refuse
  # there). Read-only: safe to call on either path.
  if [ "$2" = "$(git -C "$1" branch --show-current)" ]; then
    echo "checked out"; return 0
  fi
  if git -C "$1" worktree list --porcelain 2>/dev/null \
      | grep -qxF "branch refs/heads/$2"; then
    echo "checked out in another worktree"; return 0
  fi
  echo ""
}
clean_delete_branch() { # $1 repo $2 branch — gated explicit delete, no -d/-D
  # Gate (the check `-d` used to smuggle in): fully merged into
  # refs/heads/main. Re-verified immediately before the write so the check
  # and the delete cannot drift apart (a concurrent merge only ever ADDS
  # eligibility; history rewrites are out of scope for a local cleanup).
  git -C "$1" merge-base --is-ancestor "$2" refs/heads/main 2>/dev/null \
    || fail "clean: refusing to delete $2 (not merged into main)"
  # Live-tree guards: same predicate the dry-run listing uses, so the two
  # paths cannot disagree. update-ref on a live checkout would strand HEAD
  # (`git branch -d` refused these too).
  local block_reason; block_reason=$(clean_delete_guard "$1" "$2")
  [ -z "$block_reason" ] \
    || fail "clean: refusing to delete $2 ($block_reason)"
  # Compare-and-swap on the resolved tip: if the ref moved after the
  # merged-ness gate above (new commits, concurrent cleaner), the delete
  # fails loud instead of removing a tip nobody checked (#425 hardening).
  local old_tip
  old_tip=$(git -C "$1" rev-parse --verify "refs/heads/$2" 2>/dev/null) \
    || fail "clean: refusing to delete $2 (ref vanished during delete)"
  git -C "$1" update-ref -d "refs/heads/$2" "$old_tip" \
    || fail "clean: refusing to delete $2 (ref moved during delete)"
}
__clean_pred() { # $1 branch -> owned|protected|foreign (introspection hook)
  if is_protected_branch "$1"; then echo protected; return; fi
  if is_owned_branch "$1"; then echo owned; return; fi
  echo foreign
}
mode_clean() {
  [ $# -ge 1 ] && [ $# -le 2 ] || fail "usage: clean <issue> [--dry-run]"
  local issue dry=""; issue=$(clean_issue_num "$1")
  if [ $# -eq 2 ]; then
    [ "$2" = "--dry-run" ] || fail "usage: clean <issue> [--dry-run]"
    dry="--dry-run"
  fi
  __clean_repo "$REPO_ROOT" "$issue" "$dry"
}

mode_watch() {
  [ $# -ge 1 ] || fail "usage: watch <issue> [arm|disarm|tick|status] [interval_s]"
  local issue act
  issue=$(clean_issue_num "$1"); shift
  act="status"
  if [ $# -ge 1 ]; then
    case "$1" in
      arm|disarm|tick|status) act="$1"; shift;;
      *) fail "usage: watch <issue> [arm|disarm|tick|status] [interval_s]";;
    esac
  fi
  if [ "$act" = "arm" ]; then
    [ $# -le 1 ] || fail "usage: watch <issue> arm [interval_s]"
  else
    [ $# -eq 0 ] || fail "usage: watch <issue> [arm|disarm|tick|status] [interval_s]"
  fi
  export NEXIOS_AGENT_UP="$SCRIPT_DIR/agent_up.sh"
  export NEXIOS_DEV_SESS="$DEV_SESS" NEXIOS_QE_SESS="$QE_SESS"
  export NEXIOS_DOORBELL_SESS="$ENGINE_SESS"
  exec "$SCRIPT_DIR/watchdog.sh" "$act" "$issue" "$@"
}

mode_close() {
  local all=0; [ "${1:-}" = "--all" ] && all=1
  for s in "$DEV_SESS" "$QE_SESS" "$AUDIT_SESS"; do
    if tmux has-session -t "$s" 2>/dev/null; then tmux kill-session -t "$s"; info "killed $s"; fi
  done
  if [ "$all" = 1 ]; then
    for s in "$CONSULT_SESS" "$ENGINE_SESS" "nexios-coord"; do
      if tmux has-session -t "$s" 2>/dev/null; then tmux kill-session -t "$s"; info "killed $s"; fi
    done
  fi
}

mode="${1:-}"; shift || true
case "$mode" in
  setup) mode_setup "$@" ;;
  workers) mode_workers "$@" ;;
  up) mode_up "$@" ;;
  run) mode_run "$@" ;;
  audit) mode_audit "$@" ;;
  audit-diff) mode_audit_diff "$@" ;;
  qe-merge) mode_qe_merge "$@" ;;
  msg) mode_msg "$@" ;;
  wake) mode_wake "$@" ;;
  consult) mode_consult "$@" ;;
  windows) mode_windows "$@" ;;
  state) mode_state_set "$@" ;;
  comms-test) mode_comms_test "$@" ;;
  status) mode_status "$@" ;;
  self-test) mode_self_test "$@" ;;
  render-self-test) mode_render_self_test "$@" ;;
  fence) mode_fence_show "$@" ;;
  __check-clean-tree) mode_check_clean_tree "$@" ;;
  __run-cmd) mode_launch_cmd "$@" ;;
  __room-plan) mode_room_plan "$@" ;;
  __consult-cmd) CONSULT_TMPDIR=/tmp/agentup-tmp-probe; consult_tui_cmd ;;
  approve) mode_approve "$@" ;;
  close) mode_close "$@" ;;
  watch) mode_watch "$@" ;;
  clean) mode_clean "$@" ;;
  __clean-repo) __clean_repo "$@" ;;
  __clean-pred) __clean_pred "$@" ;;
  *) usage ;;
esac
