#!/usr/bin/env bash
# watchdog.sh — in-repo dev/qe activity watchdog (operator-ordered lane).
# Runtime files live ONLY in /tmp/nexios-<issue>/. Never writes into the repo.
# Verbs: tick <issue> | arm <issue> [interval_s=180] | disarm <issue> |
#        status <issue>. POSIX-ish bash, no new deps (pgrep/ps/diff/tmux/git).
set -euo pipefail

DEV_SESS="${NEXIOS_DEV_SESS:-nexios-dev}"
QE_SESS="${NEXIOS_QE_SESS:-nexios-qe}"
AUDIT_SESS="${NEXIOS_AUDIT_SESS:-nexios-audit}"
COORD_SESS="${NEXIOS_COORD_SESS:-nexios-coord}"
AGENT_UP="${NEXIOS_AGENT_UP:-$(cd "$(dirname "$0")" && pwd)/agent_up.sh}"

fail() { echo "watchdog: ERROR: $*" >&2; exit 1; }
info() { echo "watchdog: $*"; }

room_for() { # $1 canonical issue -> dir, room must exist
  case "$1" in ''|*[!0-9]*) fail "issue must be numeric: '$1'";; esac
  [ "$1" != "0" ] || fail "issue must be numeric: '$1'"
  local d; d="/tmp/nexios-$((10#$1))"
  [ -d "$d" ] || fail "no room $d (start it first)"
  printf '%s' "$d"
}

sess_state() { # $1 tmux session -> pane/opencode/cpu or absent marker
  local pane op cpu
  pane=$(tmux list-panes -t "$1" -F '#{pane_pid}' 2>/dev/null | head -n 1 || true)
  [ -n "${pane:-}" ] || { echo "absent"; return 0; }
  op=$(pgrep -P "$pane" 2>/dev/null | head -n 1 || true)
  [ -n "${op:-}" ] || { echo "pane=$pane opencode=absent"; return 0; }
  cpu=$(ps -o %cpu= -p "$op" 2>/dev/null | tr -d ' ' || true)
  # Integer-truncated: raw %cpu flickers sample-to-sample (0.0 <-> 0.4) and
  # uses the locale decimal separator (0,0 here) — either breaks UNCHANGED
  # streaks on an idle worker. Idle stays 0; real activity still moves it.
  cpu=${cpu:-?}; cpu=${cpu%%[.,]*}
  echo "pane=$pane opencode=$op cpu=${cpu:-?}"
}

repo_state() { # $1 dir -> branch/head/dirty or absent
  [ -d "$1/.git" ] || { echo "absent"; return 0; }
  local br hd dd
  br=$(git -C "$1" branch --show-current 2>/dev/null || true)
  hd=$(git -C "$1" rev-parse --short HEAD 2>/dev/null || true)
  dd=$(git -C "$1" status --porcelain 2>/dev/null | wc -l | tr -d ' ' || true)
  echo "branch=${br:-?} head=${hd:-?} dirty=${dd:-?}"
}

hist_last() { # $1 repo dir -> last non-empty test-history row or none
  [ -f "$1/test-history.txt" ] || { echo "none"; return 0; }
  local last; last=$(grep -v '^$' "$1/test-history.txt" | tail -n 1 || true)
  echo "${last:-none}"
}

qemu_count() {
  local q; q=$(pgrep -c -f 'qemu-system' 2>/dev/null || true)
  echo "${q:-0}"
}

latest_report() { # $1 room -> newest audit report for this issue, or none
  # The auditor has no working doorbell; verdicts land as files.
  # The coordinator polls this (doctrine) instead of waiting for a ring.
  local issue rep
  issue=$(basename "$1"); issue=${issue#nexios-}
  rep=$(ls -t /Users/arnold/jarvis/audits/report-*-"${issue}".md /Users/arnold/jarvis/audits/report-*-"${issue}"-*.md 2>/dev/null | head -n 1 || true)
  echo "${rep:-none}"
}

fingerprint() { # $1 room -> full multi-line fingerprint on stdout
  # Self-test fixture seam (same pattern as NEXIOS_GH_BIN): when set, the
  # fingerprint is read from this file instead of live probes, so the
  # baseline/unchanged/changed/escalate state machine is deterministic
  # even while real workers are active. Never set on live paths.
  if [ -n "${NEXIOS_WATCHDOG_FAKE_FP:-}" ]; then
    cat "$NEXIOS_WATCHDOG_FAKE_FP"
    return 0
  fi
  local box bf n last
  # Worker-authored lines ONLY: the coordinator's own FROM=coord orders
  # live in these same boxes and must never tick the room (a tick means
  # a worker posted, never that the coordinator wrote).
  for box in dev qe auditor; do
    bf="$1/inbox/$box.md"
    if [ -f "$bf" ]; then
      n=$(grep -cE 'FROM=(dev|qe|audit) ' "$bf" 2>/dev/null || true)
      last=$(grep -E 'FROM=(dev|qe|audit) ' "$bf" 2>/dev/null | tail -n 1 || true)
    else
      n="no-inbox"; last="none"
    fi
    echo "inbox_${box}_lines=${n:-0}"
    echo "inbox_${box}_last=${last:-none}"
  done
  echo "[dev]"
  echo "sess=$(sess_state "$DEV_SESS")"
  echo "repo=$(repo_state "$HOME/jarvis")"
  echo "hist=$(hist_last "$HOME/jarvis")"
  echo "[qe]"
  echo "sess=$(sess_state "$QE_SESS")"
  echo "repo=$(repo_state "$HOME/jarvis_test")"
  echo "hist=$(hist_last "$HOME/jarvis_test")"
  echo "qemu=$(qemu_count)"
  echo "[auditor]"
  echo "sess=$(sess_state "$AUDIT_SESS")"
  echo "audit=$(latest_report "$1")"
}

mode_tick() { # $1 canonical issue -> verdict line (+delta), updates state
  local room fp now cnt verdict delta ndiff n
  room=$(room_for "$1")
  fp="$room/watch.fingerprint"; now="$room/watch.now"; cnt="$room/watch.unchanged"
  fingerprint "$room" > "$now.tmp"
  mv "$now.tmp" "$now"
  if [ ! -f "$fp" ]; then
    cp "$now" "$fp"; echo 0 > "$cnt"
    echo "BASELINE $1"
    return 0
  fi
  if cmp -s "$fp" "$now"; then
    n=$(($(cat "$cnt" 2>/dev/null || echo 0)+1)); echo "$n" > "$cnt"
    verdict="UNCHANGED $1 x$n"
    [ "$n" -ge 3 ] && verdict="$verdict ESCALATE"
    echo "$verdict"
    return 0
  fi
  delta=$(diff "$fp" "$now" || true)
  ndiff=$(printf '%s\n' "$delta" | wc -l | tr -d ' ')
  verdict="CHANGED $1 ($ndiff lines differ)"
  cp "$now" "$fp"; echo 0 > "$cnt"
  echo "$verdict"
  printf '%s\n' "$delta" | head -n 20
}

mode_arm() { # $1 issue [$2 interval_s]
  local room int pidf pid
  room=$(room_for "$1")
  int="${2:-180}"
  case "$int" in ''|*[!0-9]*) fail "interval must be numeric seconds: '$int'";; esac
  [ "$int" -ge 1 ] || fail "interval must be numeric seconds: '$int'"
  pidf="$room/watch.pid"
  if [ -f "$pidf" ]; then
    pid=$(cat "$pidf" 2>/dev/null || true)
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
      fail "already armed (pid $pid) — disarm first"
    fi
    rm -f "$pidf"
  fi
  nohup "$0" __ticker "$1" "$int" >/dev/null 2>&1 &
  pid=$!
  kill -0 "$pid" 2>/dev/null || fail "ticker failed to start"
  echo "$pid" > "$pidf"
  info "armed $1 every ${int}s (pid $pid)"
}

mode_disarm() { # $1 issue
  local room pidf pid ticker
  room=$(room_for "$1")
  pidf="$room/watch.pid"
  [ -f "$pidf" ] || fail "not armed: no $pidf"
  pid=$(cat "$pidf" 2>/dev/null || true)
  ticker=$(ps -o args= -p "${pid:-0}" 2>/dev/null || true)
  case "$ticker" in *__ticker*) ;; *) ticker="";; esac
  if [ -z "${pid:-}" ] || ! kill -0 "$pid" 2>/dev/null || [ -z "$ticker" ]; then
    rm -f "$pidf"
    info "stale pid file removed (ticker already dead)"
    return 0
  fi
  kill "$pid" 2>/dev/null || fail "cannot kill ticker pid $pid"
  rm -f "$pidf"
  info "disarmed $1 (killed $pid)"
}

mode_status() { # $1 issue
  local room pidf pid streak
  room=$(room_for "$1")
  pidf="$room/watch.pid"
  streak=$(cat "$room/watch.unchanged" 2>/dev/null || echo 0)
  if [ -f "$pidf" ]; then
    pid=$(cat "$pidf" 2>/dev/null || true)
    if [ -n "${pid:-}" ] && kill -0 "$pid" 2>/dev/null; then
      echo "armed pid=$pid streak=$streak"
      return 0
    fi
  fi
  echo "disarmed streak=$streak"
}

__tick_alarm() { # $1 issue (internal: one tick + exactly one doorbell attempt)
  local room out first line
  room=$(room_for "$1")
  if ! out=$("$0" tick "$1" 2>&1); then
    echo "DOORBELL-SKIPPED tick-failed $1" >> "$room/watch.log"
    return 1
  fi
  first=$(printf '%s\n' "$out" | head -n 1)
  line="inbox: TICK — $first — read watch.log"
  if "$AGENT_UP" msg "$COORD_SESS" "$line" >/dev/null 2>&1; then
    echo "silent: TICK — $first — read watch.silent.log" >> "$room/watch.silent.log"
  else
    echo "DOORBELL-FAILED $first" >> "$room/watch.silent.log"
  fi
}

__ticker() { # $1 issue $2 interval (internal: detached loop)
  trap 'exit 0' TERM INT
  while true; do
    sleep "$2"
    __tick_alarm "$1" || true
  done
}

mode="${1:-}"; shift || true
case "$mode" in
  tick) [ $# -eq 1 ] || fail "usage: watchdog.sh tick <issue>"; mode_tick "$1" ;;
  arm) ([ $# -eq 1 ] || [ $# -eq 2 ]) || fail "usage: watchdog.sh arm <issue> [interval_s]"; mode_arm "$@" ;;
  disarm) [ $# -eq 1 ] || fail "usage: watchdog.sh disarm <issue>"; mode_disarm "$1" ;;
  status) [ $# -eq 1 ] || fail "usage: watchdog.sh status <issue>"; mode_status "$1" ;;
  __tick_alarm) [ $# -eq 1 ] || fail "usage: watchdog.sh __tick_alarm <issue>"; __tick_alarm "$1" ;;
  __ticker) [ $# -eq 2 ] || fail "usage: watchdog.sh __ticker <issue> <interval>"; __ticker "$@" ;;
  *) fail "usage: watchdog.sh tick|arm|disarm|status <issue> [interval_s]" ;;
esac
