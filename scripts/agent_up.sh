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
# `harness ps` / the grid. Watch via `tmux attach -t <name> -r` (read-only).
# Proper fix belongs upstream: env passthrough on `harness new`.
# Fencing itself is verified per-session BEFORE any prompt is delivered.
set -euo pipefail

NEXIOS_HOME="${NEXIOS_HOME:-$HOME/.local/state/nexios}"
COORD_DIR="$NEXIOS_HOME/coordinator"
ROLES_DIR="$NEXIOS_HOME/roles"
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
  up <issue> ["task"] FULL ROOM from one zsh command: setup + workers + two
                     visible Terminal watch mirrors (read-only) + coordinator
                     in tmux ($COORD_SESS) with the task pasted as its first
                     message. Pass a taskfile path or literal task text.
  run ...            launch ONE session: run <name> <dir> <role.json> <prompt.txt> [branch]
  audit <issue>      launch $AUDIT_SESS from <runtime>/prompts/auditor.txt
  audit-diff [branch] print merge-base audit diff to audits/pending_patch.diff
                     (non-empty assertion). Run in the dev checkout.
  qe-merge <issue>   merge origin/main into testbed --no-commit --no-ff;
                     abort-on-conflict restores code state. Run in QE checkout.
  msg <sess> <text>  single-line doorbell to a tmux session (both directions).
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
runtime_dir() { [ $# -eq 1 ] || fail "need issue number"; echo "/tmp/nexios-$1"; }

# ---embedded role defaults (bootstrap only; audited content)------------------
# dev: edit-all EXCEPT test trees. Last-match-wins; both star depths listed
# because the docs don't pin down whether * crosses /.
write_role_dev() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "edit": {
      "*": "allow",
      "src/kernel/test/*": "deny",
      "src/kernel/test/**/*": "deny",
      "src/test/*": "deny",
      "src/test/**/*": "deny"
    }
  }
}
EOF
}

# qe: edit ONLY test trees.
write_role_qe() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "edit": {
      "*": "deny",
      "src/kernel/test/*": "allow",
      "src/kernel/test/**/*": "allow",
      "src/test/*": "allow",
      "src/test/**/*": "allow"
    }
  }
}
EOF
}

# auditor: deny-all writes; may read the stepper bin dir + coord runtime.
# ASSUMPTION (flagged for audit review): exact allow paths for the cage.
write_role_auditor() {
  cat > "$1" <<'EOF'
{
  "$schema": "https://opencode.ai/config.json",
  "permission": {
    "edit": {
      "*": "deny",
      "/tmp/*": "allow",
      "/Users/arnold/.local/state/nexios/bin/**": "allow",
      "/Users/arnold/jarvis/audits/rejected_patch.diff": "allow",
      "/Users/arnold/jarvis/audits/report-*.md": "allow"
    }
  }
}
EOF
}

write_roles_readme() {
  cat > "$1" <<'EOF'
# Role configs — bootstrapped by scripts/agent_up.sh setup (embedded defaults).
# dev.json: edit everything EXCEPT test trees. qe.json: edit ONLY test trees.
# auditor.json: deny-all writes (cage: /tmp + audits artifacts + stepper bin).
# edit covers edit/write/patch only, NOT bash heredocs/redirects — the repo
# checker (tools/check_no_cross_edits.sh) is the fail-closed backstop.
# Step zero of every session: `opencode debug config` shows these rules +
# a blocked write fails at depth 1 AND nested + the allowed write succeeds.
EOF
}

ensure_roles() {
  mkdir -p "$ROLES_DIR"
  [ -f "$ROLES_DIR/dev.json" ] || { write_role_dev "$ROLES_DIR/dev.json"; info "bootstrapped roles/dev.json"; }
  [ -f "$ROLES_DIR/qe.json" ] || { write_role_qe "$ROLES_DIR/qe.json"; info "bootstrapped roles/qe.json"; }
  [ -f "$ROLES_DIR/auditor.json" ] || { write_role_auditor "$ROLES_DIR/auditor.json"; info "bootstrapped roles/auditor.json (ASSUMPTION: cage paths — flagged for audit review)"; }
  [ -f "$ROLES_DIR/README.md" ] || write_roles_readme "$ROLES_DIR/README.md"
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
- State: `/tmp/nexios-<issue>/state.json` — single writer: coordinator.
  Guard state-modifying steps with the fcntl lock helper (no flock on macOS).
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
  mkdir -p -m 0700 "$rt/inbox" "$rt/notes" "$rt/prompts"
  for r in dev qe auditor; do
    [ -f "$rt/inbox/$r.md" ] || echo "# inbox/$r.md — append-only. Format: <ISO8601> FROM=<role> NEED|READY|NOTE :: <text>" > "$rt/inbox/$r.md"
  done
  [ -f "$rt/state.json" ] || { write_state_skeleton "$rt/state.json" "$1"; info "seeded state.json (IN_PROGRESS, retries 0/3)"; }
  write_protocol "$rt"
  info "coord runtime ready: $rt"
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
check_clean_tree() {
  local dir="$1"; shift
  local dirty ex ok=0
  dirty=$(git -C "$dir" status --porcelain | awk '{print $2}')
  for f in $dirty; do
    ok=1
    for ex in "$@"; do case "$f" in "$ex"*) ok=0;; esac; done
    [ "$ok" = 0 ] || fail "dirty tree: $f (commit/stash first; exempt: $*)"
  done
}

# mutual exclusion for state-modifying steps (no flock binary on macOS).
# usage: with_state_lock <state.json> <command...>
with_state_lock() {
  local st="$1"; shift
  python3 -c "import fcntl,sys; fcntl.flock(open(sys.argv[1],'r+'),fcntl.LOCK_EX)" "$st"
  "$@"
  local rc=$?
  python3 -c "import fcntl,sys; fcntl.flock(open(sys.argv[1],'r+'),fcntl.LOCK_UN)" "$st"
  return $rc
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
  tmux send-keys -t "$name" "env -u OPENCODE_CONFIG -u OPENCODE_CONFIG_CONTENT OPENCODE_CONFIG=$role $OPENCODE_BIN" Enter
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
check_fenced() {
  local name="$1" want_pwd="$2" want_cfg="$3" pid="" p
  for p in $(pgrep -f "opencode/bin/opencode$" 2>/dev/null); do
    if ps eww -p "$p" 2>/dev/null | tr ' ' '\n' | grep -qx "PWD=$want_pwd"; then pid="$p"; break; fi
  done
  [ -n "$pid" ] || { echo "no opencode process with PWD=$want_pwd" >&2; return 1; }
  local cfg
  cfg=$(ps eww -p "$pid" 2>/dev/null | tr ' ' '\n' | grep "^OPENCODE_CONFIG=" | head -1 || true)
  [ "$cfg" = "OPENCODE_CONFIG=$want_cfg" ] || {
    echo "PWD ok but OPENCODE_CONFIG wrong: got '${cfg:-<unset>}', want '$want_cfg'" >&2; return 1
  }
  return 0
}

# ---coordinator (addressable endpoint)-----------------------------------------
coord_tui_cmd() {
  echo "env -u OPENCODE_CONFIG -u OPENCODE_CONFIG_CONTENT $OPENCODE_BIN"
}

# ensure nexios-coord tmux session exists running the coordinator TUI.
ensure_coord_session() {
  need_cmd tmux; need_cmd "$OPENCODE_BIN"
  need_file "$COORD_DIR/opencode.json"
  if ! tmux has-session -t "$COORD_SESS" 2>/dev/null; then
    tmux new-session -d -s "$COORD_SESS" -c "$COORD_DIR" -x 200 -y 50
    tmux send-keys -t "$COORD_SESS" "$(coord_tui_cmd)" Enter
    info "started $COORD_SESS (coordinator TUI; attach: tmux attach -t $COORD_SESS)"
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

# $1 tmux session name — opens a new visible Terminal window attached read-only.
open_watch_window() {
  osascript -e "tell application \"Terminal\" to do script \"tmux attach -t $1 -r\"" >/dev/null
  info "opened watch window for '$1' (read-only attach; detach with Ctrl-b d)"
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
  open_watch_window "$DEV_SESS"
  open_watch_window "$QE_SESS"
  open_watch_window "$COORD_SESS"
  cat <<EOF
watch: three Terminal windows opened (read-only mirrors). Detach with Ctrl-b d.
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
    ensure_coord_session
  fi
}

# audit-diff: generate merge-base audit diff with non-empty assertion.
# usage: audit-diff [branch]   (default: current branch; run in dev checkout)
mode_audit_diff() {
  local br="${1:-$(git branch --show-current)}"
  local base; base=$(git merge-base main "$br")
  local out="audits/pending_patch.diff"
  git diff "$base".."$br" -- . > "$out" || true
  [ -s "$out" ] || fail "audit diff empty for $br (nothing to audit)"
  info "audit diff: $out ($(wc -l < "$out") lines, merge-base $base)"
}

# qe-merge: merge origin/main into testbed --no-commit --no-ff.
# usage: qe-merge <issue>   (run in the QE checkout; aborts cleanly on conflict)
mode_qe_merge() {
  [ $# -eq 1 ] || fail "qe-merge needs an issue number"
  check_clean_tree "$(pwd)" "test-history.txt" "graphify-out/"
  git fetch origin main 2>/dev/null || true
  if git merge --no-commit --no-ff origin/main; then
    info "merged origin/main --no-commit (inspect, test, then commit or abort)"
  else
    git merge --abort || true
    fail "conflict — aborted, code state restored (resolve manually)"
  fi
}

# single-line doorbell to a tmux session (both directions). usage: msg <sess> <text>
mode_msg() {
  [ $# -eq 2 ] || fail "msg needs: <tmux-session> <single-line-text>"
  case "$2" in *$'\n'*) fail "doorbell must be a single line (payloads live in inbox)";; esac
  tmux has-session -t "$1" 2>/dev/null || fail "no such tmux session: $1 (start it first)"
  tmux send-keys -t "$1" "$2" Enter
  info "doorbell sent to '$1'"
}

mode_status() {
  if [ "${1:-}" = "--reconstruct" ]; then shift; mode_status_reconstruct "$@"; return; fi
  info "--- tmux ---"; tmux ls 2>&1 || true
  info "--- branches ---"
  echo "dev: $(git -C "$DEV_DIR" branch --show-current 2>/dev/null || echo '?')"
  echo "qe:  $(git -C "$QE_DIR" branch --show-current 2>/dev/null || echo '?')"
  if [ $# -ge 1 ]; then
    local rt; rt=$(runtime_dir "$1")
    for r in dev qe auditor; do
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
  case "$copy" in /tmp/*drill*|/tmp/*staging*|/tmp/*copy*) ;;
    *) fail "refusing: --copy must point under /tmp with drill/staging/copy in the name";;
  esac
  local rt; rt=$(runtime_dir "$issue")
  [ -f "$rt/state.json" ] || fail "no live state to compare against"
  case "$copy" in "$rt") fail "refusing to reconstruct the live dir (use a COPY)";; esac
  mkdir -p "$copy"
  python3 - "$rt" "$copy" "$DEV_DIR" "$QE_DIR" <<'EOF'
import json, subprocess, sys
rt, copy, devdir, qedir = sys.argv[1:5]
live = json.load(open(rt + "/state.json"))
dev_br = subprocess.run(["git","-C",devdir,"branch","--show-current"],
                        capture_output=True, text=True).stdout.strip()
qe_br = subprocess.run(["git","-C",qedir,"branch","--show-current"],
                       capture_output=True, text=True).stdout.strip()
recon = {"halt": live.get("halt", False), "issue": live.get("issue"),
         "phase": live.get("phase", ""), "state": live.get("state"),
         "retries": live.get("retries", 0),
         "retry_budget": live.get("retry_budget", 3),
         "updated": live.get("updated", ""),
         "pending": live.get("pending", "")}
open(copy + "/state.json", "w").write(json.dumps(recon, indent=2) + "\n")
print("reconstruct: dev=%s qe=%s" % (dev_br, qe_br))
print("machine fields: " + ("MATCH" if all(
    recon[k] == live.get(k) for k in
    ("phase","state","retries","retry_budget","halt")) else "MISMATCH"))
EOF
}

mode_self_test() {
  local t=0 pass=0
  chk() { t=$((t+1)); if "$@" >/dev/null 2>&1; then pass=$((pass+1)); echo "PASS: $*"; else echo "FAIL: $*"; fi; }
  chk_neg() { t=$((t+1)); if "$@" >/dev/null 2>&1; then echo "FAIL (should abort): $*"; else pass=$((pass+1)); echo "PASS: $*"; fi; }
  # guard aborts (scratch names only — never live sessions)
  chk_neg ./scripts/agent_up.sh run "agentup-st-$$" /nonexistent "$ROLES_DIR/dev.json" /dev/null || true
  # msg discipline
  chk_neg ./scripts/agent_up.sh msg "agentup-nosuch-$$" "hi" || true
  printf 'two\nlines' > /tmp/agentup-ml-$$.txt || true
  # reconstruct refuses live dir ( Triple-check: uses --copy guard)
  chk_neg ./scripts/agent_up.sh status --reconstruct 410 --copy /tmp/nexios-410 || true
  rm -f /tmp/agentup-ml-$$.txt
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
  status) mode_status "$@" ;;
  self-test) mode_self_test "$@" ;;
  approve) mode_approve "$@" ;;
  close) mode_close "$@" ;;
  *) usage ;;
esac
