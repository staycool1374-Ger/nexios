#!/usr/bin/env bash
# worker-state.sh v2 — WORKING | IDLE | STUCK verdict for a tmux worker session.
# Defect fixed vs v1: raw pane-hash flips on TUI shimmer (spinners, clocks).
# v2 compares NORMALIZED content (volatile lines stripped), token deltas,
# timer deltas, and tool-child CPU. No single signal decides alone.
# Usage: worker-state.sh <session>   (e.g. nexios-dev)
set -u
sess="${1:?usage: worker-state.sh <session>}"
if ! tmux has-session -t "$sess" 2>/dev/null; then echo "$sess ABSENT (no tmux session)"; exit 0; fi
pane=$(tmux list-panes -t "$sess" -F '#{pane_pid}' 2>/dev/null | head -n 1)
[ -n "${pane:-}" ] || { echo "$sess ABSENT (no pane)"; exit 0; }
norm() { # stdin -> pane text minus volatile lines/chars
  sed -E -e 's/· [0-9]+m [0-9]+s//g' -e 's/[0-9]+\.[0-9]K \([0-9]+%\)//g' \
         -e '/Thought ·/d' -e '/ctrl\+p commands/d' -e 's/[$]0\.[0-9]+ spent//g' \
    | tr -d '⬝■◐◑◒◓▣⇆'
}
snap() { # echoes: hash|timer|tokens|childcpu
  local p h t tk cc
  p=$(tmux capture-pane -t "$sess" -p 2>/dev/null | norm)
  h=$(printf '%s' "$p" | md5sum | cut -d' ' -f1)
  t=$(tmux capture-pane -t "$sess" -p 2>/dev/null | grep -oE "· [0-9]+m [0-9]+s" | head -n 1)
  tk=$(tmux capture-pane -t "$sess" -p 2>/dev/null | grep -oE "[0-9]+\.[0-9]K \([0-9]+%\)" | head -n 1)
  cc=$(ps -o %cpu= -g "$(ps -o sid= -p "$pane" 2>/dev/null | tr -d ' ')" 2>/dev/null | awk '{s+=$1} END {printf "%d", s+0}')
  printf '%s|%s|%s|%s' "$h" "$t" "$tk" "$cc"
}
markers() { tmux capture-pane -t "$sess" -p 2>/dev/null | grep -cE "esc interrupt" || true; }
A=$(snap); sleep 20; B=$(snap); M=$(markers)
h1=${A%%|*}; rest1=${A#*|}; t1=${rest1%%|*}; rest1=${rest1#*|}; k1=${rest1%%|*}; c1=${rest1##*|}
h2=${B%%|*}; rest2=${B#*|}; t2=${rest2%%|*}; rest2=${rest2#*|}; k2=${rest2%%|*}; c2=${rest2##*|}
if [ "$h1" != "$h2" ] || [ "$k1" != "$k2" ]; then echo "$sess WORKING (content or tokens changed over 20s; timer='$t2' kids-cpu=$c2%)"; exit 0; fi
if [ "${c2:-0}" -ge 5 ]; then echo "$sess WORKING (tool children burning ${c2}% CPU; timer='$t2')"; exit 0; fi
if [ "$M" -ge 1 ] && [ -n "$t2" ]; then echo "$sess STUCK (frozen 20s with run markers, no output, no child CPU; timer='$t2' tokens='$k2') — ladder, do not re-ring"; exit 0; fi
echo "$sess IDLE (static, bare prompt; tokens='$k2')"
