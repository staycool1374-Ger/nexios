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
ROLES_DIR="${ROLES_DIR:-$REPO_ROOT/tools/agents/roles}"
COORD_CONFIG_DIR="$REPO_ROOT/tools/agents/coordinator"
NEXIOS_HOME="${NEXIOS_HOME:-$HOME/.local/state/nexios}"
COORD_DIR="$NEXIOS_HOME/coordinator"
COORD_CONFIG_RESOLVED="$COORD_DIR/opencode.resolved.json"
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

usage() {
  cat >&2 <<EOF
usage: agent_up.sh <mode> [...]
  setup <issue>      create coord runtime (/tmp/nexios-<issue>/: inbox, state,
                     prompts, notes, PROTOCOL) + bootstrap roles if missing.
                     Idempotent: never overwrites existing state.json/inbox.
  coord [taskfile]   ensure $COORD_SESS tmux session (cwd=coordinator home,
                     env scrubbed) running the coordinator TUI; attach to it.
                     Optional taskfile is pasted as the first message.
  workers <issue>    launch $DEV_SESS + $QE_SESS (detached tmux; needs
                     prompts/dev.txt + prompts/qe.txt in the coord runtime).
  up <issue> ["task"] FULL ROOM from one zsh command: setup + workers + three
                     visible Terminal windows (read-write mirrors; read-only
                     mirrors block ALL tmux input including doorbells) + coordinator
                     in tmux ($COORD_SESS) with the task pasted as its first
                     message. Pass a taskfile path or literal task text.
  run ...            launch ONE session: run <name> <dir> <role.json> <prompt.txt> [branch]
  audit <issue>      launch $AUDIT_SESS from <runtime>/prompts/auditor.txt
  audit-diff [branch] [--base <rev>]
                     write the audit diff to audits/pending_patch.diff
                     (base defaults to merge-base main <branch>; must be
                     non-empty). Run in the dev checkout.
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
  printf '%s\n' "$dir"
}

# ---embedded role defaults (bootstrap only; audited content)------------------
# dev: edit-all EXCEPT test trees + own cage. Byte-identical to
# tools/agents/roles/dev.json (canonical); this copy is the broken-checkout
# fallback only. Last-match-wins; both star depths (docs don't pin * crossing /).
write_role_dev() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "bash": "allow",
    "external_directory": {
      "*": "deny",
      "/tmp/nexios-*": "allow",
      "/private/tmp/nexios-*": "allow",
      "/tmp/nexios-*/**": "allow",
      "/private/tmp/nexios-*/**": "allow",
      "/tmp/jarvis-*": "allow",
      "/private/tmp/jarvis-*": "allow",
      "/tmp/jarvis-*/**": "allow",
      "/private/tmp/jarvis-*/**": "allow",
      "/tmp/jarvis_*": "allow",
      "/private/tmp/jarvis_*": "allow",
      "/tmp/matrix-*": "allow",
      "/private/tmp/matrix-*": "allow",
      "/tmp/qemu-serial.*": "allow",
      "/private/tmp/qemu-serial.*": "allow",
      "/tmp/agentup-*": "allow",
      "/private/tmp/agentup-*": "allow",
      "/tmp/agentup-*/**": "allow",
      "/private/tmp/agentup-*/**": "allow",
      "/tmp/lldb-*": "allow",
      "/private/tmp/lldb-*": "allow"
    },
    "edit": {
      "*": "allow",
      "tools/agents/roles/*": "deny",
      "tools/agents/roles/**/*": "deny",
      "src/kernel/test/*": "deny",
      "src/kernel/test/**/*": "deny",
      "src/test/*": "deny",
      "src/test/**/*": "deny"
    }
  }
}
EOF
}

# qe: edit ONLY test trees (+coord mailbox). Byte-identical to
# tools/agents/roles/qe.json (canonical); fallback only.
write_role_qe() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "bash": "allow",
    "external_directory": {
      "*": "deny",
      "/tmp/nexios-*": "allow",
      "/private/tmp/nexios-*": "allow",
      "/tmp/nexios-*/**": "allow",
      "/private/tmp/nexios-*/**": "allow",
      "/tmp/jarvis-*": "allow",
      "/private/tmp/jarvis-*": "allow",
      "/tmp/jarvis-*/**": "allow",
      "/private/tmp/jarvis-*/**": "allow",
      "/tmp/jarvis_*": "allow",
      "/private/tmp/jarvis_*": "allow",
      "/tmp/matrix-*": "allow",
      "/private/tmp/matrix-*": "allow",
      "/tmp/qemu-serial.*": "allow",
      "/private/tmp/qemu-serial.*": "allow",
      "/tmp/agentup-*": "allow",
      "/private/tmp/agentup-*": "allow",
      "/tmp/agentup-*/**": "allow",
      "/private/tmp/agentup-*/**": "allow",
      "/tmp/lldb-*": "allow",
      "/private/tmp/lldb-*": "allow"
    },
    "edit": {
      "*": "deny",
      "/tmp/nexios-*/*": "allow",
      "/private/tmp/nexios-*/*": "allow",
      "/tmp/nexios-*/**/*": "allow",
      "/private/tmp/nexios-*/**/*": "allow",
      "src/kernel/test/*": "allow",
      "src/kernel/test/**/*": "allow",
      "src/test/*": "allow",
      "src/test/**/*": "allow"
    }
  }
}
EOF
}

# auditor: deny-all writes except mailbox + audit artifacts. Byte-identical to
# tools/agents/roles/auditor.json (canonical); fallback only.
write_role_auditor() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "bash": "allow",
    "external_directory": {
      "*": "deny",
      "/tmp/nexios-*": "allow",
      "/private/tmp/nexios-*": "allow",
      "/tmp/nexios-*/**": "allow",
      "/private/tmp/nexios-*/**": "allow",
      "/tmp/jarvis-*": "allow",
      "/private/tmp/jarvis-*": "allow",
      "/tmp/jarvis-*/**": "allow",
      "/private/tmp/jarvis-*/**": "allow",
      "/tmp/jarvis_*": "allow",
      "/private/tmp/jarvis_*": "allow",
      "/tmp/matrix-*": "allow",
      "/private/tmp/matrix-*": "allow",
      "/tmp/qemu-serial.*": "allow",
      "/private/tmp/qemu-serial.*": "allow",
      "/tmp/agentup-*": "allow",
      "/private/tmp/agentup-*": "allow",
      "/tmp/agentup-*/**": "allow",
      "/private/tmp/agentup-*/**": "allow",
      "/tmp/lldb-*": "allow",
      "/private/tmp/lldb-*": "allow"
    },
    "edit": {
      "*": "deny",
      "/tmp/nexios-*/*": "allow",
      "/private/tmp/nexios-*/*": "allow",
      "/tmp/nexios-*/**/*": "allow",
      "/private/tmp/nexios-*/**/*": "allow",
      "audits/rejected_patch.diff": "allow",
      "audits/report-*.md": "allow"
    }
  }
}
EOF
}

# coordinator: deny-all writes except mailbox + LEARNINGS closeout.
# Byte-identical to tools/agents/roles/coordinator.json (canonical).
write_role_coordinator() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "bash": "allow",
    "external_directory": {
      "*": "deny",
      "{repo}/prompts/**": "allow",
      "{repo}/audits/**": "allow",
      "/tmp/nexios-*": "allow",
      "/private/tmp/nexios-*": "allow",
      "/tmp/nexios-*/**": "allow",
      "/private/tmp/nexios-*/**": "allow",
      "/tmp/jarvis-*": "allow",
      "/private/tmp/jarvis-*": "allow",
      "/tmp/jarvis-*/**": "allow",
      "/private/tmp/jarvis-*/**": "allow",
      "/tmp/jarvis_*": "allow",
      "/private/tmp/jarvis_*": "allow",
      "/tmp/matrix-*": "allow",
      "/private/tmp/matrix-*": "allow",
      "/tmp/qemu-serial.*": "allow",
      "/private/tmp/qemu-serial.*": "allow",
      "/tmp/agentup-*": "allow",
      "/private/tmp/agentup-*": "allow",
      "/tmp/agentup-*/**": "allow",
      "/private/tmp/agentup-*/**": "allow",
      "/tmp/lldb-*": "allow",
      "/private/tmp/lldb-*": "allow"
    },
    "edit": {
      "*": "deny",
      "/tmp/nexios-*/*": "allow",
      "/private/tmp/nexios-*/*": "allow",
      "/tmp/nexios-*/**/*": "allow",
      "/private/tmp/nexios-*/**/*": "allow",
      "{repo}/prompts/LEARNINGS.md": "allow"
    }
  }
}
EOF
}
write_roles_readme() {
  cat > "$1" <<'EOF'
# Role configs — canonical content lives in tools/agents/roles/ (repo).
# This embedded copy is the broken-checkout fallback ONLY (used when the
# repo files are absent); keep byte-identical with the repo files.
# dev.json: edit-all EXCEPT test trees + own cage. qe.json: edit ONLY test
# trees + coord mailbox. auditor.json: deny-all writes except mailbox + audit
# artifacts. coordinator.json: deny-all except mailbox + LEARNINGS closeout.
# edit covers edit/write/patch only, NOT bash heredocs/redirects — the repo
# checker (tools/check_no_cross_edits.sh) is the fail-closed backstop.
# Step zero of every session: `opencode debug config` shows these rules +
# a blocked write fails at depth 1 AND nested + the allowed write succeeds.
EOF
}

ensure_roles() {
  mkdir -p "$ROLES_DIR"
  [ -f "$ROLES_DIR/dev.json" ] || { write_role_dev "$ROLES_DIR/dev.json"; info "bootstrapped roles/dev.json (fallback — repo copy canonical)"; }
  [ -f "$ROLES_DIR/qe.json" ] || { write_role_qe "$ROLES_DIR/qe.json"; info "bootstrapped roles/qe.json (fallback — repo copy canonical)"; }
  [ -f "$ROLES_DIR/auditor.json" ] || { write_role_auditor "$ROLES_DIR/auditor.json"; info "bootstrapped roles/auditor.json (fallback — repo copy canonical)"; }
  [ -f "$ROLES_DIR/coordinator.json" ] || { write_role_coordinator "$ROLES_DIR/coordinator.json"; info "bootstrapped roles/coordinator.json (fallback — repo copy canonical)"; }
  [ -f "$ROLES_DIR/README.md" ] || write_roles_readme "$ROLES_DIR/README.md"
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

# State dir + legacy role paths may contain LINKS ONLY (canonical: repo).
ensure_state_links() {
  ensure_link "$COORD_DIR/opencode.json" "$COORD_CONFIG_DIR/opencode.json"
  ensure_link "$COORD_DIR/AGENTS.md" "$COORD_CONFIG_DIR/AGENTS.md"
  ensure_link "$NEXIOS_HOME/roles/dev.json" "$ROLES_DIR/dev.json"
  ensure_link "$NEXIOS_HOME/roles/qe.json" "$ROLES_DIR/qe.json"
  ensure_link "$NEXIOS_HOME/roles/auditor.json" "$ROLES_DIR/auditor.json"
  ensure_link "$NEXIOS_HOME/roles/coordinator.json" "$ROLES_DIR/coordinator.json"
  ensure_link "$NEXIOS_HOME/roles/README.md" "$ROLES_DIR/README.md"
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
  ensure_roles
  ensure_state_links
  render_coord_config
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

# $1 name, $2 dir, $3 role json, $4 prompt file, $5 expected branch (empty = skip)
mode_run() {
  [ $# -ge 4 ] || fail "run needs: <name> <dir> <role.json> <prompt.txt> [branch]"
  local name="$1"
  local dir="$2"
  local role="$3"
  local prompt="$4"
  local want_branch="${5:-}"
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  need_file "$role"; need_file "$prompt"
  [ -d "$dir" ] || fail "workdir missing: $dir"
  check_branch "$dir" "$want_branch"
  run_session "$name" "$dir" "$role" "$prompt"
}

mode_audit() {
  [ $# -eq 1 ] || fail "audit needs an issue number"
  local rt; rt=$(runtime_dir "$1")
  need_file "$rt/prompts/auditor.txt"
  need_file "$ROLES_DIR/auditor.json"
  local branch=""
  if [ -f "$rt/state.json" ]; then
    branch=$(python3 -c "import json;print(json.load(open('$rt/state.json')).get('audit_branch',''))")
  fi
  # Auditor reads the dev checkout (diff + files), never writes: cwd=~/jarvis.
  mode_run "$AUDIT_SESS" "$DEV_DIR" "$ROLES_DIR/auditor.json" "$rt/prompts/auditor.txt" "$branch"
}

# $1 tmux-name, $2 workdir, $3 role json, $4 prompt file
# (split locals: bash 3.2 expands all RHS before assigning under set -u)
run_session() {
  local name="$1"
  local dir="$2"
  local role="$3"
  local prompt="$4"
  [ -d "$dir" ] || fail "workdir missing: $dir"
  need_file "$role"; need_file "$prompt"
  if tmux has-session -t "$name" 2>/dev/null; then
    fail "tmux session '$name' already exists — refusing to clobber (close it first)"
  fi
  tmux new-session -d -s "$name" -c "$dir" -x 200 -y 50
  # Env hygiene: role config ONLY from OPENCODE_CONFIG; strip any leaky overrides.
  # TMPDIR pinned inside the fenced namespace (host TMPDIR is per-boot: unfenceable).
  local tdir; tdir=$(ensure_session_tmpdir "$name")
  tmux send-keys -t "$name" "env -u OPENCODE_CONFIG -u OPENCODE_CONFIG_CONTENT TMPDIR=$tdir OPENCODE_CONFIG=$role $OPENCODE_BIN" Enter
  sleep "$TUI_BOOT_WAIT"
  check_fenced "$name" "$dir" "$role" || {
    tmux kill-session -t "$name" 2>/dev/null || true
    fail "session '$name' launched UNFENCED (config not in process env) — killed, fix before retry"
  }
  tmux load-buffer "$prompt"
  tmux paste-buffer -t "$name"
  sleep 2
  tmux send-keys -t "$name" Enter
  info "launched '$name' (fenced, prompt delivered)"
}

# $1 tmux-name, $2 expected PWD, $3 expected role config
# Scoped to the tmux pane's own process tree: dev and auditor share PWD, so a
# global PWD match can select the sibling's opencode (false UNFENCED + kill).
check_fenced() {
  local name="$1" want_pwd="$2" want_cfg="$3"
  local pane pid i found=0 seen=""
  pane=$(tmux list-panes -t "$name" -F '#{pane_pid}' 2>/dev/null | head -1)
  [ -n "$pane" ] || { echo "no tmux pane for '$name'" >&2; return 1; }
  local pids=("$pane")
  i=0
  while [ "$i" -lt "${#pids[@]}" ]; do
    for pid in $(pgrep -P "${pids[$i]}" 2>/dev/null); do pids[${#pids[@]}]="$pid"; done
    i=$((i + 1))
  done
  for pid in "${pids[@]}"; do
    local env="" pwd_v cfg_v
    env=$(ps eww -p "$pid" 2>/dev/null) || continue
    case "$env" in *"$OPENCODE_BIN"*) ;; *) continue ;; esac
    pwd_v=$(printf '%s\n' "$env" | tr ' ' '\n' | grep "^PWD=" | head -1 || true)
    cfg_v=$(printf '%s\n' "$env" | tr ' ' '\n' | grep "^OPENCODE_CONFIG=" | head -1 || true)
    seen="${seen}pid $pid pwd=${pwd_v:-?} cfg=${cfg_v:-<unset>}; "
    [ "$pwd_v" = "PWD=$want_pwd" ] && [ "$cfg_v" = "OPENCODE_CONFIG=$want_cfg" ] && { found=1; break; }
  done
  [ "$found" = 1 ] && return 0
  echo "pane tree of '$name' has no opencode with PWD=$want_pwd + OPENCODE_CONFIG=$want_cfg (saw: ${seen:-<none>})" >&2
  return 1
}

# ---coordinator (addressable endpoint)-----------------------------------------
# The repo template carries a literal {repo} token instead of a machine path
# (relative rules under the coordinator cwd resolve to a shadow file: S3-2R).
# It is rendered into an out-of-repo resolved config at launch.
render_coord_config() {
  need_cmd python3
  need_file "$COORD_CONFIG_DIR/opencode.json"
  mkdir -p "$COORD_DIR"
  local repo_real; repo_real=$(cd "$REPO_ROOT" && pwd -P)
  python3 - "$COORD_CONFIG_DIR/opencode.json" "$COORD_CONFIG_RESOLVED" "$repo_real" <<'EOF'
import json, os, sys


def die(msg):
    sys.stderr.write("agent_up: ERROR: %s\n" % msg)
    sys.exit(1)


src, dst, repo = sys.argv[1:4]
if not os.path.isabs(repo):
    die("repo root is not absolute: %r" % repo)
if not os.path.isdir(os.path.join(repo, "prompts")):
    die("no prompts/ under the repo root %s (wrong checkout?)" % repo)
try:
    with open(src) as handle:
        raw = handle.read()
except OSError as exc:
    die("cannot read %s: %s" % (src, exc.strerror or exc))
if "{repo}" not in raw:
    die("template has no {repo} token: %s" % src)
out = raw.replace("{repo}", repo)
if "{repo}" in out:
    die("unresolved {repo} token in the rendered config")
try:
    json.loads(out)
except ValueError as exc:
    die("rendered config is not valid JSON: %s" % exc)
tmp = dst + ".tmp"
with open(tmp, "w") as handle:
    handle.write(out)
    handle.flush()
    os.fsync(handle.fileno())
os.replace(tmp, dst)
print("agent_up: coordinator config rendered: %s" % dst)
EOF
}

coord_tui_cmd() {
  echo "env -u OPENCODE_CONFIG_CONTENT TMPDIR=$COORD_TMPDIR OPENCODE_CONFIG=$COORD_CONFIG_RESOLVED $OPENCODE_BIN"
}

# ensure nexios-coord tmux session exists running the coordinator TUI.
ensure_coord_session() {
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  ensure_state_links
  render_coord_config
  need_file "$COORD_CONFIG_RESOLVED"
  if tmux has-session -t "$COORD_SESS" 2>/dev/null; then
    # Never trust a session this launch did not create: a pre-fence
    # coordinator (launched with no OPENCODE_CONFIG at all) or a moved repo
    # leaves a running session whose env is not the resolved config. Workers
    # refuse reuse outright; the coordinator kills the stale session so the
    # block below rebuilds it fenced (S4-9: reuse must assert too).
    check_fenced "$COORD_SESS" "$COORD_DIR" "$COORD_CONFIG_RESOLVED" 2>/dev/null || {
      tmux kill-session -t "$COORD_SESS" 2>/dev/null || true
    }
  fi
  if ! tmux has-session -t "$COORD_SESS" 2>/dev/null; then
    COORD_TMPDIR=$(ensure_session_tmpdir "$COORD_SESS")
    tmux new-session -d -s "$COORD_SESS" -c "$COORD_DIR" -x 200 -y 50
    tmux send-keys -t "$COORD_SESS" "$(coord_tui_cmd)" Enter
    sleep "$TUI_BOOT_WAIT"
    check_fenced "$COORD_SESS" "$COORD_DIR" "$COORD_CONFIG_RESOLVED" || {
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
mode_workers() {
  [ $# -eq 1 ] || fail "workers needs an issue number"
  local rt; rt=$(runtime_dir "$1")
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  need_file "$rt/prompts/dev.txt"; need_file "$rt/prompts/qe.txt"
  need_file "$ROLES_DIR/dev.json"; need_file "$ROLES_DIR/qe.json"
  check_branch "$DEV_DIR" main
  check_branch "$QE_DIR" testbed
  run_session "$DEV_SESS" "$DEV_DIR" "$ROLES_DIR/dev.json" "$rt/prompts/dev.txt"
  run_session "$QE_SESS" "$QE_DIR" "$ROLES_DIR/qe.json" "$rt/prompts/qe.txt"
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
mode_up() {
  [ $# -ge 1 ] || fail "up needs an issue number: up <issue> [taskfile-or-task]"
  local issue="$1"
  local task="${2:-}"
  mode_setup "$issue"
  local rt; rt=$(runtime_dir "$issue")
  mode_workers "$issue"
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
  else
    echo "(no task given — attach and paste: 'You are nexios-coord. Read AGENTS.md. Report readiness.')"
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
      python3 -c "import json; s=json.load(open('$rt/state.json')); print({k: s.get(k) for k in ('phase','state','retries','retry_budget','halt','pending')})"
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
  chk_neg "$SCRIPT_DIR/agent_up.sh" run "agentup-st-$$" /nonexistent "$ROLES_DIR/dev.json" /dev/null || true
  # msg discipline
  chk_neg "$SCRIPT_DIR/agent_up.sh" msg "agentup-nosuch-$$" "hi" || true
  printf 'two\nlines' > /tmp/agentup-ml-$$.txt || true
  # reconstruct refuses live dir ( Triple-check: uses --copy guard)
  chk_neg "$SCRIPT_DIR/agent_up.sh" status --reconstruct 410 --copy /tmp/nexios-410 || true
  rm -f /tmp/agentup-ml-$$.txt
  # fence configuration: families byte-identical, mirrors complete, no drift
  chk python3 "$REPO_ROOT/tools/agents/verify_fence.py"
  # the coordinator launch command carries the RESOLVED config + pinned TMPDIR
  # (S4-9: the fence assertion is only meaningful if the env is the fenced one)
  local cc; cc=$("$SCRIPT_DIR/agent_up.sh" __coord-cmd)
  chk bash -c 'printf "%s" "$1" | grep -q "OPENCODE_CONFIG=.*coordinator/opencode.resolved.json"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q "TMPDIR=/tmp/agentup-tmp-"' _ "$cc"
  chk bash -c 'printf "%s" "$1" | grep -q -- "-u OPENCODE_CONFIG_CONTENT"' _ "$cc"
  # issue-number canonicality: 0, over-long and leading-zero aliases
  chk_neg "$SCRIPT_DIR/agent_up.sh" setup 0
  chk_neg "$SCRIPT_DIR/agent_up.sh" setup 12345678
  # out-of-repo policy: a REAL file where a link belongs must hard-fail (drift),
  # an absent path must be linked, a wrong link must be repointed.
  local st="/tmp/agentup-st-$$"; mkdir -p "$st/home/coordinator"
  printf 'drift\n' > "$st/home/coordinator/opencode.json"
  chk_neg env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9999
  rm -f "$st/home/coordinator/opencode.json"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9999
  chk test -L "$st/home/coordinator/opencode.json"
  chk test "$(readlink "$st/home/coordinator/opencode.json")" = "$COORD_CONFIG_DIR/opencode.json"
  chk test -L "$st/home/roles/dev.json"
  # a WRONG link is repointed, never adopted
  ln -sfn /nonexistent "$st/home/coordinator/opencode.json"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup 9999
  chk test "$(readlink "$st/home/coordinator/opencode.json")" = "$COORD_CONFIG_DIR/opencode.json"
  # leading-zero issue numbers canonicalize onto ONE runtime (never alias 410)
  local zcan
  local zraw
  zcan=$((900000 + ($$ % 90000)))
  zraw="000$zcan"
  chk env NEXIOS_HOME="$st/home" "$SCRIPT_DIR/agent_up.sh" setup "$zraw"
  chk test -f "/tmp/nexios-$zcan/state.json"
  chk_neg test -e "/tmp/nexios-$zraw"
  # coordinator config is rendered from the {repo} template, template untouched
  chk test -f "$st/home/coordinator/opencode.resolved.json"
  chk grep -q '{repo}' "$COORD_CONFIG_DIR/opencode.json"
  chk_neg grep -q '{repo}' "$st/home/coordinator/opencode.resolved.json"
  chk grep -q "$(cd "$REPO_ROOT" && pwd -P)/prompts/LEARNINGS.md" \
      "$st/home/coordinator/opencode.resolved.json"
  chk python3 -c "import json,sys; json.load(open(sys.argv[1]))" \
      "$st/home/coordinator/opencode.resolved.json"
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
  __check-clean-tree) mode_check_clean_tree "$@" ;;
  __coord-cmd) COORD_TMPDIR=/tmp/agentup-tmp-probe; coord_tui_cmd ;;
  approve) mode_approve "$@" ;;
  close) mode_close "$@" ;;
  *) usage ;;
esac
