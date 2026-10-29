#!/usr/bin/env bash
# worker-state.sh v3 — WORKING | IDLE | STUCK | BLOCKED verdict for a tmux worker session.
# v2 defect (fixed): a rendered permission modal counts as pane turnover, so a
# worker blocked on input read as WORKING. v3 checks for the modal FIRST.
# Order: modal? -> normalized-content change? -> token change? -> child CPU? ->
#          frozen+markers? -> idle. Never infers from CPU or tokens alone.
# Usage: worker-state.sh <session>   (e.g. nexios-dev)
set -u
sess="${1:?usage: worker-state.sh <session>}"
if ! tmux has-session -t "$sess" 2>/dev/null; then echo "$sess ABSENT (no tmux session)"; exit 0; fi
pane=$(tmux list-panes -t "$sess" -F '#{pane_pid}' 2>/dev/null | head -n 1)
[ -n "${pane:-}" ] || { echo "$sess ABSENT (no pane)"; exit 0; }
MODAL_RE='allow once|allow always|permission required|needs approval|always allow|enter confirm|reject permission|tell opencode what to do differently|needs your decision|awaiting approval'
norm() {
  sed -E -e 's/· [0-9]+m [0-9]+s//g' -e 's/[0-9]+\.[0-9]K \([0-9]+%\)//g' \
    -e '/Thought ·/d' -e '/ctrl\+p commands/d' -e 's/[$]0\.[0-9]+ spent//g' \
    | tr -d '⬝■◐◑◒◓▣⇆'
}
snap() { # echoes: hash|timer|tokens|childcpu|modalhits
  local p h t tk cc mm
  p=$(tmux capture-pane -t "$sess" -p 2>/dev/null)
  mm=$(printf '%s' "$p" | grep -ciE "$MODAL_RE" || true)
  h=$(printf '%s' "$p" | norm | md5sum | cut -d' ' -f1)
  t=$(printf '%s' "$p" | grep -oE "· [0-9]+m [0-9]+s" | head -n 1)
  tk=$(printf '%s' "$p" | grep -oE "[0-9]+\.[0-9]K \([0-9]+%\)" | head -n 1)
  cc=$(ps -o %cpu= -g "$(ps -o sid= -p "$pane" 2>/dev/null | tr -d ' ')" 2>/dev/null | awk '{s+=$1} END {printf "%d", s+0}')
  printf '%s|%s|%s|%s|%s' "$h" "$t" "$tk" "$cc" "$mm"
}
markers() { tmux capture-pane -t "$sess" -p 2>/dev/null | grep -c "esc interrupt" || true; }
A=$(snap); sleep 20; B=$(snap); M=$(markers)
h1=${A%%|*}; r1=${A#*|}; t1=${r1%%|*}; r1=${r1#*|}; k1=${r1%%|*}; r1=${r1#*|}; c1=${r1%%|*}; m1=${r1##*|}
h2=${B%%|*}; r2=${B#*|}; t2=${r2%%|*}; r2=${r2#*|}; k2=${r2%%|*}; r2=${r2#*|}; c2=${r2%%|*}; m2=${r2##*|}
if [ "${m1:-0}" -ge 1 ] || [ "${m2:-0}" -ge 1 ]; then echo "$sess BLOCKED (permission prompt visible — dismiss per deny-policy, do not re-ring)"; exit 0; fi
if [ "$h1" != "$h2" ] || [ "$k1" != "$k2" ]; then echo "$sess WORKING (content or tokens changed over 20s; timer='$t2' kids-cpu=$c2%)"; exit 0; fi
if [ "${c2:-0}" -ge 5 ]; then echo "$sess WORKING (tool children burning ${c2}% CPU; timer='$t2')"; exit 0; fi
if [ "$M" -ge 1 ] && [ -n "$t2" ]; then echo "$sess STUCK (frozen 20s with run markers, no output, no child CPU; timer='$t2' tokens='$k2') — ladder, do not re-ring"; exit 0; fi
echo "$sess IDLE (static, bare prompt; tokens='$k2')"
