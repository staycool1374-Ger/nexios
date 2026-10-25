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
#   coordinator named tmux session (nexios-coord) — the STABLE ADDRESS every
#              doorbell rings. The coordinator had no address (bare TUI in an
#              anonymous terminal), so worker→coordinator wakes were impossible
#              and a human relayed every transition. Fixed here by construction.
#   comms      coord runtime root /tmp/nexios-<issue>/ (inbox/dev|qe|auditor.md,
#              state.json, prompts/, notes/, PROTOCOL.md), role configs
#              bootstrapped from embedded defaults if absent.
#
# DOORBELL (both directions, tmux send-keys — harness cannot see tmux sessions,
# so `harness send`/agent-messages is NOT a wake path here):
#   coord→worker:  agent_up.sh msg nexios-dev "inbox: READY from dev — read inbox/qe.md"
#   worker→coord:  agent_up.sh msg nexios-coord "inbox: dev READY — read inbox/dev.md"
# Rules (both ways): single line, inbox POINTER only (payloads never in chat),
# idle sessions only, never touch permission dialogs (approve mode is person-run).
# The coordinator treats any ring as "re-read the inbox", never executes ring text.
#
# WHY TMUX, NOT `harness new`: fenced sessions need OPENCODE_CONFIG in their
# process env. `harness new` talks to the daemon over IPC, which drops the
# invoker's env (no --env flag), so harness-managed sessions launch UNFENCED
# (proven: S1 step-zero fails there). Direct tmux launch inherits our env, so
# fences hold (proven: S1 passes). Tradeoff: tmux sessions are invisible to
# `harness ps` / the grid. Watch via `tmux attach -t <name>` (read-write mirrors;
# read-only (-r) mirrors block ALL tmux input including doorbells, so none here).
# Proper fix belongs upstream: env passthrough on `harness new`.
# Fencing itself is verified per-session BEFORE any prompt is delivered, for the
# coordinator too (`S4-9`): a session whose env lacks the expected config is
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
COORD_TMPDIR=""
OPENCODE_BIN="${OPENCODE_BIN:-$HOME/.opencode/bin/opencode}"
TUI_BOOT_WAIT="${TUI_BOOT_WAIT:-12}"
DEV_DIR="$HOME/jarvis"
QE_DIR="$HOME/jarvis_test"
COORD_SESS="nexios-coord"
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
COORD_CONFIG_RESOLVED="$(rendered_path coordinator)"

usage() {
  cat >&2 <<EOF
usage: agent_up.sh <mode> [...]
  setup <issue>      create coord runtime (/tmp/nexios-<issue>/: inbox, state,
                     prompts, notes, PROTOCOL) + render every role fence into
                     $RENDERED_ROOT. Idempotent: never overwrites state/inbox.
  coord [taskfile]   ensure $COORD_SESS tmux session (cwd=coordinator home,
                     env scrubbed) running the coordinator TUI; attach to it.
                     Optional taskfile is pasted as the first message.
  workers [issue]    launch $DEV_SESS + $QE_SESS (detached tmux). With an issue
                     they get prompts/dev.txt + prompts/qe.txt from the coord
                     runtime; without one they start bare and wait for a ring.
  up [issue] ["task"] FULL ROOM from one zsh command: setup (only if an issue is
                     given) + workers + three visible Terminal windows
                     (read-write mirrors; read-only mirrors block ALL tmux input
                     including doorbells) + coordinator in tmux ($COORD_SESS)
                     with the task pasted as its first message. Bare 'up' opens
                     the room: type the issue number or your idea into the
                     coordinator window; it runs setup and briefs the workers.
  run ...            launch ONE session: run <name> <dir> <role> <prompt.txt> [branch]
                     <role> is dev|qe|auditor|coordinator — the fence for it is
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
  wake <sess> <text> alias of msg (same guard + single-line rule).
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
                     touches $DEV_SESS/$QE_SESS/$AUDIT_SESS/$COORD_SESS).
  approve <sess>     drive one Allow-always on the session's CURRENT permission
                     dialog (checks first; person-run only).
  close [--all]      kill worker tmux sessions; --all also kills $COORD_SESS.
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
  for role in dev qe auditor coordinator; do
    render_role_config "$role" >/dev/null
  done
  info "fence rendered for dev, qe, auditor, coordinator under $RENDERED_ROOT"
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
  for role in dev qe auditor; do
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
    fail "$link holds a REAL file from the pre-#414 layout; the fence is now rendered from $FENCE_DIR. Move or delete it, then re-run."
  fi
}

# State dir links: AGENTS.md (coordinator doctrine). Config files are RENDERED
# into $RENDERED_ROOT and never linked; pre-#414 links/role copies are pruned.
ensure_state_links() {
  mkdir -p "$NEXIOS_HOME"
  ensure_link "$COORD_DIR/AGENTS.md" "$REPO_ROOT/tools/agents/coordinator/AGENTS.md"
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
  The coordinator treats any ring as "re-read the inbox", never executes it.
- Coordinator address: tmux session `nexios-coord` (created by `coord`/`up`).
- State: `/tmp/nexios-<issue>/state.json` — single writer: coordinator, and the
  only sanctioned write path is `agent_up.sh state <issue> key=value` (python3
  fcntl lock on `state.lock`, atomic replace + fsync, keys/vocabulary validated).
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
           "retry_budget", "state", "updated"}
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
        elif key == "halt":
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
  case "$role" in dev|qe|auditor|coordinator) ;; *) fail "run takes a role name (dev|qe|auditor|coordinator), not a config path: '$role'" ;; esac
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
  case "$role" in dev|qe|auditor|coordinator) ;; *) fail "unknown role: '$role'" ;; esac
  if tmux has-session -t "$name" 2>/dev/null; then
    fail "tmux session '$name' already exists — refusing to clobber (close it first)"
  fi
  # install_agents installs the generated subagent definitions into the
  # session's cwd; render_role_config then re-renders THIS role fresh. Never
  # trust the file on disk here: a pre-planted or stale config under
  # $RENDERED_ROOT must never launch (the renderer validates the role and
  # overwrites with a fresh compose, fail-closed on any error).
  install_agents "$dir"
  local cfg; cfg=$(render_role_config "$role")
  tmux new-session -d -s "$name" -c "$dir" -x 200 -y 50
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
  tmux send-keys -t "$name" "env -u OPENCODE_CONFIG -u OPENCODE_CONFIG_CONTENT TMPDIR=$tdir OPENCODE_CONFIG=$cfg NEXIOS_AGENT_ROLE=$role $OPENCODE_BIN" Enter
  sleep "$TUI_BOOT_WAIT"
  check_fenced "$name" "$dir" "$cfg" "$role" || {
    tmux kill-session -t "$name" 2>/dev/null || true
    fail "session '$name' launched UNFENCED (config/role/tmpdir not in process env) — killed, fix before retry"
  }
  if [ -n "$prompt" ]; then
    tmux load-buffer "$prompt"
    tmux paste-buffer -t "$name"
    sleep 2
    tmux send-keys -t "$name" Enter
    info "launched '$name' (fenced, prompt delivered)"
  else
    info "launched '$name' (fenced, no brief yet — the coordinator rings it)"
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

# ---coordinator (addressable endpoint)-----------------------------------------
# The coordinator fence is a role delta like every other one: `{repo}` tokens
# are expanded to THIS checkout at render time, so a moved clone cannot leave a
# stale machine path behind (S3-2R). Rendered path comes from rendered_path().
coord_tui_cmd() {
  local v
  for v in "$COORD_TMPDIR" "$COORD_CONFIG_RESOLVED"; do
    case "$v" in
      *[[:space:]]*) fail "coordinator launch path contains whitespace, which the fenced env line cannot express: '$v'" ;;
    esac
  done
  echo "env -u OPENCODE_CONFIG_CONTENT TMPDIR=$COORD_TMPDIR OPENCODE_CONFIG=$COORD_CONFIG_RESOLVED NEXIOS_AGENT_ROLE=coordinator $OPENCODE_BIN"
}

# ensure nexios-coord tmux session exists running the coordinator TUI.
ensure_coord_session() {
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  ensure_state_links
  render_role_config coordinator >/dev/null
  need_file "$COORD_CONFIG_RESOLVED"
  if tmux has-session -t "$COORD_SESS" 2>/dev/null; then
    # Never trust a session this launch did not create: a pre-fence
    # coordinator (launched with no OPENCODE_CONFIG at all) or a moved repo
    # leaves a running session whose env is not the resolved config. Workers
    # refuse reuse outright; the coordinator kills the stale session so the
    # block below rebuilds it fenced (S4-9: reuse must assert too).
    check_fenced "$COORD_SESS" "$COORD_DIR" "$COORD_CONFIG_RESOLVED" coordinator 2>/dev/null || {
      tmux kill-session -t "$COORD_SESS" 2>/dev/null || true
    }
  fi
  if ! tmux has-session -t "$COORD_SESS" 2>/dev/null; then
    COORD_TMPDIR=$(ensure_session_tmpdir "$COORD_SESS")
    tmux new-session -d -s "$COORD_SESS" -c "$COORD_DIR" -x 200 -y 50
    tmux send-keys -t "$COORD_SESS" "$(coord_tui_cmd)" Enter
    sleep "$TUI_BOOT_WAIT"
    check_fenced "$COORD_SESS" "$COORD_DIR" "$COORD_CONFIG_RESOLVED" coordinator || {
      tmux kill-session -t "$COORD_SESS" 2>/dev/null || true
      fail "coordinator launched UNFENCED (resolved config not in its env) — killed, fix before retry"
    }
    info "started $COORD_SESS (fenced, config=$COORD_CONFIG_RESOLVED)"
  fi
}

# $1 optional taskfile to paste as the coordinator's first message
mode_coord() {
  [ -n "${GITHUB_PERSONAL_ACCESS_TOKEN:-}" ] || fail "GITHUB_PERSONAL_ACCESS_TOKEN unset (github MCP needs it)"
  if [ -n "${OPENCODE_CONFIG:-}${OPENCODE_CONFIG_CONTENT:-}" ]; then
    info "scrubbing leaky OPENCODE_CONFIG* from coordinator env"
  fi
  ensure_coord_session
  if [ $# -ge 1 ]; then
    need_file "$1"
    tmux load-buffer "$1"
    tmux paste-buffer -t "$COORD_SESS"
    sleep 2
    tmux send-keys -t "$COORD_SESS" Enter
    info "delivered first message to $COORD_SESS"
  fi
  if [ -n "${TMUX:-}" ]; then
    tmux switch-client -t "$COORD_SESS"
  else
    tmux attach -t "$COORD_SESS"
  fi
}

# ---modes----------------------------------------------------------------------
# workers [issue] — without an issue the sessions start bare (no brief): the
# coordinator writes the briefs after setup and rings each worker. Briefs are
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
  check_branch "$DEV_DIR" main
  check_branch "$QE_DIR" testbed
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

# Full room from one zsh command: setup + workers + visible watch mirrors +
# coordinator in tmux (addressable doorbell endpoint) with the task pasted.
# up [issue] ["task"] — the issue is optional: `up` alone opens the room and
# you type the issue number or your idea into the coordinator window.
mode_up() {
  [ $# -le 2 ] || fail "up takes at most: up [issue] [taskfile-or-task] (got $#)"
  local issue="" task=""
  if [ $# -ge 1 ]; then
    # runtime_dir is the validator: a non-numeric first argument is an error,
    # never an "idea" (use bare `up` and type the idea in the coordinator window).
    runtime_dir "$1" >/dev/null
    issue="$1"
    task="${2:-}"
    mode_setup "$issue"
  fi
  mode_workers ${issue:+"$issue"}
  ensure_coord_session
  open_watch_window "$DEV_SESS"
  open_watch_window "$QE_SESS"
  open_watch_window "$COORD_SESS"
  cat <<EOF
watch: three Terminal windows opened (read-write mirrors). Detach with Ctrl-b d.
watch: Harness.app grid stays empty by design (tmux sessions are not discovered).
       Do NOT start opencode in app panes (that would duplicate live workers).
next: the coordinator runs in tmux session '$COORD_SESS' (the doorbell address).
EOF
  if [ -n "$task" ]; then
    if [ -f "$task" ]; then
      mode_coord "$task"
    else
      local tf; tf=$(mktemp)
      printf '%s\n' "$task" > "$tf"
      mode_coord "$tf"
      rm -f "$tf"
    fi
  elif [ -n "$issue" ]; then
    echo "(no task given — attach and paste: 'You are nexios-coord. Read AGENTS.md. Report readiness.')"
  else
    echo "(no issue selected — no runtime created. In the coordinator window type the"
    echo " issue number or your idea; it runs 'setup <n>', writes both briefs and rings"
    echo " the workers: 'agent_up.sh msg nexios-dev \"inbox: brief ready\"')"
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

# End-to-end transport self-test: proves msg delivers text+Enter INTO a live
# tmux pane in both directions, using two scratch echo sessions (no opencode,
# no tokens, no $DEV_SESS/$QE_SESS/$COORD_SESS touched).
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
  # coord -> worker direction / worker -> coord direction (same mode_msg path)
  mode_msg "$a" "PING-$$" >/dev/null || ok=0
  mode_msg "$b" "PONG-$$" >/dev/null || ok=0
  sleep 1
  tmux capture-pane -p -t "$a" | grep -q "GOT:PING-$$" || { echo "FAIL: coord->worker not delivered"; ok=0; }
  tmux capture-pane -p -t "$b" | grep -q "GOT:PONG-$$" || { echo "FAIL: worker->coord not delivered"; ok=0; }
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
  chk bash -c 'printf "%s" "$1" | grep -qx "sessions=nexios-dev,nexios-qe,nexios-coord"' _ "$rp"
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
  # the coordinator launch command carries the RESOLVED config + pinned TMPDIR
  # (S4-9: the fence assertion is only meaningful if the env is the fenced one)
  local cc; cc=$("$SCRIPT_DIR/agent_up.sh" __coord-cmd)
  chk bash -c 'printf "%s" "$1" | grep -q "OPENCODE_CONFIG=.*coordinator/opencode.resolved.json"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q "TMPDIR=/tmp/agentup-tmp-"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q -- "-u OPENCODE_CONFIG_CONTENT"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q "NEXIOS_AGENT_ROLE=coordinator"' _ "$cc"
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
  chk_neg "$SCRIPT_DIR/agent_up.sh" __run-cmd coordinator/../dev
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
  chk test -L "$st/home/coordinator/AGENTS.md"
  chk test "$(readlink "$st/home/coordinator/AGENTS.md")" = "$REPO_ROOT/tools/agents/coordinator/AGENTS.md"
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
  # every role is rendered by setup; coordinator {repo} keys are expanded
  local rr
  for rr in dev qe auditor coordinator; do
    chk test -f "$st/home/rendered/$rr/opencode.resolved.json"
  done
  chk_neg grep -q '{repo}' "$st/home/rendered/coordinator/opencode.resolved.json"
  chk grep -q "$(cd "$REPO_ROOT" && pwd -P)/prompts/LEARNINGS.md" \
      "$st/home/rendered/coordinator/opencode.resolved.json"
  chk python3 -c "import json,sys; json.load(open(sys.argv[1]))" \
      "$st/home/rendered/coordinator/opencode.resolved.json"
  # state writer: validated keys/vocabulary, locked, atomic
  chk_neg "$SCRIPT_DIR/agent_up.sh" state 9999 bogus=1
  chk_neg "$SCRIPT_DIR/agent_up.sh" state 9999 state=NONSENSE
  chk_neg "$SCRIPT_DIR/agent_up.sh" state 9999 retries=abc
  chk "$SCRIPT_DIR/agent_up.sh" state 9999 phase=AUDITED retries=1
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
      [ $# -eq 2 ] || fail "fence show needs a role (dev|qe|auditor|coordinator)"
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
    "$DEV_SESS" "$QE_SESS" "$COORD_SESS"
}

mode_launch_cmd() {
  [ $# -eq 1 ] || fail "__run-cmd needs a role"
  case "$1" in dev|qe|auditor|coordinator) ;; *) fail "__run-cmd needs a role (dev|qe|auditor|coordinator), got '$1'" ;; esac
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
  coord_ok() {
    python3 - "$1" "$REPO_ROOT" <<'PY'
import json, sys
cfg = json.load(open(sys.argv[1]))
repo = sys.argv[2]
assert "mcp" in cfg and "github" in cfg["mcp"], "mcp lost"
edit = cfg["permission"]["edit"]
ext = cfg["permission"]["external_directory"]
repos = {k: v for k, v in list(edit.items()) + list(ext.items()) if k.startswith(repo)}
assert set(repos) == {repo + "/prompts/LEARNINGS.md", repo + "/prompts/**",
                      repo + "/audits/**"}, repos
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
  for r in dev qe auditor coordinator; do
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
  # R7 mirrors, R8 host TMPDIR, R9 coordinator keys, R10 worker purity
  for r in dev qe auditor coordinator; do
    chk mirrors_ok "$out/$r/opencode.resolved.json"
    chk no_host_tmp "$out/$r/opencode.resolved.json"
  done
  chk coord_ok "$out/coordinator/opencode.resolved.json"
  for r in dev qe auditor; do
    chk worker_ok "$out/$r/opencode.resolved.json" "$REPO_ROOT"
  done
  # R11 generated twins match their rendered map
  chk twin_ok "$out/dev/nexios-dev.md" "$out/dev/opencode.resolved.json"
  chk twin_ok "$out/qe/nexios-qe.md" "$out/qe/opencode.resolved.json"
  chk twin_ok "$out/auditor/nexios-audit.md" "$out/auditor/opencode.resolved.json"
  chk_neg test -e "$out/coordinator/nexios-coordinator.md"

  # N1-N13 fail-closed probes
  chk_neg python3 "$RENDERER" --role bogus --print --repo-root "$REPO_ROOT" --fence-root "$FENCE_DIR"
  local d
  d=$(fixture n2); rm -f "$d/roles/qe.json"
  chk_neg python3 "$RENDERER" --role qe --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n3); mutate "$d/roles/dev.json" 'd["edit_deny"].append("{repo}/src/x")'
  chk_neg python3 "$RENDERER" --role dev --print --repo-root "$REPO_ROOT" --fence-root "$d"
  d=$(fixture n4); mutate "$d/roles/coordinator.json" 'd["edit_allow"].append("prompts/LEARNINGS.md")'
  chk_neg python3 "$RENDERER" --role coordinator --print --repo-root "$REPO_ROOT" --fence-root "$d"
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

mode_close() {
  local all=0; [ "${1:-}" = "--all" ] && all=1
  for s in "$DEV_SESS" "$QE_SESS" "$AUDIT_SESS"; do
    if tmux has-session -t "$s" 2>/dev/null; then tmux kill-session -t "$s"; info "killed $s"; fi
  done
  if [ "$all" = 1 ]; then
    if tmux has-session -t "$COORD_SESS" 2>/dev/null; then tmux kill-session -t "$COORD_SESS"; info "killed $COORD_SESS"; fi
  fi
}

mode="${1:-}"; shift || true
case "$mode" in
  setup) mode_setup "$@" ;;
  coord) mode_coord "$@" ;;
  workers) mode_workers "$@" ;;
  up) mode_up "$@" ;;
  run) mode_run "$@" ;;
  audit) mode_audit "$@" ;;
  audit-diff) mode_audit_diff "$@" ;;
  qe-merge) mode_qe_merge "$@" ;;
  msg) mode_msg "$@" ;;
  wake) mode_msg "$@" ;;
  state) mode_state_set "$@" ;;
  comms-test) mode_comms_test "$@" ;;
  status) mode_status "$@" ;;
  self-test) mode_self_test "$@" ;;
  render-self-test) mode_render_self_test "$@" ;;
  fence) mode_fence_show "$@" ;;
  __check-clean-tree) mode_check_clean_tree "$@" ;;
  __run-cmd) mode_launch_cmd "$@" ;;
  __room-plan) mode_room_plan "$@" ;;
  __coord-cmd) COORD_TMPDIR=/tmp/agentup-tmp-probe; coord_tui_cmd ;;
  approve) mode_approve "$@" ;;
  close) mode_close "$@" ;;
  *) usage ;;
esac
