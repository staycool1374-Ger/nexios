#!/usr/bin/env bash
# worker-state.sh v4 — WORKING | IDLE | STUCK | BLOCKED verdict for a tmux worker session.
# v3 defect (fixed): a rendered permission modal counts as pane turnover, so a
# worker blocked on input read as WORKING. v3 checks for the modal FIRST.
# v4 defects fixed (#462): norm() deleted spinner glyphs, Thought lines and
# timers (activity signals), the timer regex missed seconds-only "· 23.6s",
# and lifetime-average %cpu floored live builds to IDLE. v4 norm strips only
# rendering noise (ANSI/trailing space/blank lines); activity is pane-hash
# delta OR timer/token delta over the window OR session CPU-time delta; IDLE
# only on a proven-static window. Order: modal? -> content change? ->
# timer advance? -> frozen+markers? -> markers? -> child CPU delta? -> idle.
# Usage: worker-state.sh <session>   (e.g. nexios-dev)
#        worker-state.sh --selftest   (fixture matrix, no tmux, no sleeps)
set -u
TIMER_RE='· ([0-9]+m )?[0-9]+(\.[0-9]+)?s'
TOKENS_RE='[0-9]+\.[0-9]K \([0-9]+%\)'
MODAL_RE='allow once|allow always|permission required|needs approval|always allow|enter confirm|reject permission|tell opencode what to do differently|needs your decision|awaiting approval'
norm() {
  sed -e 's/\x1b\[[0-9;]*[a-zA-Z]//g' -e 's/[[:space:]]*$//' -e '/^[[:space:]]*$/d'
}
timer_of() { grep -oE "$TIMER_RE" | head -n 1; }
tokens_of() { grep -oE "$TOKENS_RE" | head -n 1; }
tosec() { # $1 cputime ([[dd-]hh:]mm:ss[.frac]) -> whole seconds; 0 on garbage
  awk -v s="$1" 'BEGIN{d=0; n=split(s,a,"-"); if(n==2){d=a[1]; s=a[2]}
    n=split(s,a,":"); h=0; m=0; sec=0;
    if(n==3){h=a[1]; m=a[2]; sec=a[3]} else if(n==2){m=a[1]; sec=a[2]} else{sec=a[1]}
    printf "%d", d*86400+h*3600+m*60+sec}'
}
cpu_total() { # $1 sid -> summed cputime seconds of the process group
  ps -o cputime= -g "$1" 2>/dev/null | while read -r c _; do tosec "$c"; echo; done |
    awk '{s+=$1} END {printf "%d", s+0}'
}
classify() { # $1 paneA $2 paneB $3 cpuA $4 cpuB $5 sess-name -> verdict line
  pa=$1; pb=$2; ca=$3; cb=$4; sess=$5
  if printf '%s\n%s' "$pa" "$pb" | grep -qiE "$MODAL_RE"; then
    printf '%s BLOCKED (permission prompt visible — dismiss per deny-policy, do not re-ring)' "$sess"; return
  fi
  ha=$(printf '%s' "$pa" | norm | md5sum | cut -d' ' -f1)
  hb=$(printf '%s' "$pb" | norm | md5sum | cut -d' ' -f1)
  ka=$(printf '%s' "$pa" | tokens_of); kb=$(printf '%s' "$pb" | tokens_of)
  ta=$(printf '%s' "$pa" | timer_of); tb=$(printf '%s' "$pb" | timer_of)
  if [ "$ha" != "$hb" ] || [ "$ka" != "$kb" ]; then
    printf '%s WORKING (content or tokens changed over window; timer=%s kids-cpu %ss->%ss)' "$sess" "$tb" "$ca" "$cb"; return
  fi
  if [ -n "$tb" ] && [ "$ta" != "$tb" ]; then
    printf '%s WORKING (timer advanced %s -> %s; kids-cpu %ss->%ss)' "$sess" "$ta" "$tb" "$ca" "$cb"; return
  fi
  case "$pa$pb" in *"esc interrupt"*) markers=1;; *) markers=0;; esac
  if [ "$markers" -ge 1 ] && [ -n "$tb" ]; then
    # Run markers visible but the timer did not advance and no child CPU
    # burned: the run is wedged mid-generation. Distinct from WORKING
    # (which re-rings) — STUCK ladders, never re-rings.
    if [ -z "${ca:-}" ] || [ -z "${cb:-}" ] || [ "$cb" -le "$ca" ] 2>/dev/null; then
      printf '%s STUCK (frozen window with run markers, no output, no child CPU; timer=%s tokens=%s) — ladder, do not re-ring' "$sess" "$tb" "$kb"; return
    fi
  fi
  if [ "$markers" -ge 1 ]; then
    printf '%s WORKING (run markers visible; timer=%s kids-cpu %ss->%ss)' "$sess" "$tb" "$ca" "$cb"; return
  fi
  if [ -n "${ca:-}" ] && [ -n "${cb:-}" ] && [ "$cb" -gt "$ca" ] 2>/dev/null; then
    pct=$(( (cb - ca) * 100 / ${WIN:-20} ))
    if [ "$pct" -ge 5 ]; then
      printf '%s WORKING (tool children burning %ss CPU over %ss window; timer=%s)' "$sess" "$((cb-ca))" "${WIN:-20}" "$tb"; return
    fi
  fi
  printf '%s IDLE (static, bare prompt; tokens=%s)' "$sess" "$kb"
}
if [ "${1:?usage: worker-state.sh <session> | --selftest}" = "--selftest" ]; then
  pass=0; total=0
  chk() { # $1 name $2 got-substr-must-contain $3 got
    total=$((total+1))
    case "$3" in *"$2"*) pass=$((pass+1)); echo "PASS $1";; *)
      echo "FAIL $1 (want '*$2*', got: $3)";; esac
  }
  A_IDLE='❯  '
  chk "static-idle" "IDLE (" "$(classify "$A_IDLE" "$A_IDLE" 100 100 t)"
  A_T1='opencode · 23.6s 47.0K (84%) ❯ thinking'; B_T1='opencode · 24.6s 47.0K (84%) ❯ thinking'
  chk "timer-seconds-detected" "WORKING (" "$(classify "$A_T1" "$B_T1" 100 100 t)"
  A_TM='opencode · 3m 12s ❯ x'; B_TM='opencode · 3m 13s ❯ x'
  chk "timer-minutes-detected" "WORKING (" "$(classify "$A_TM" "$B_TM" 100 100 t)"
  A_SP='⬝ working'; B_SP='■ working'
  chk "spinner-rotation-working" "WORKING (" "$(classify "$A_SP" "$B_SP" 100 100 t)"
  A_TH='❯ x'; B_TH='Thought · 23.6s
❯ x'
  chk "thought-appears-working" "WORKING (" "$(classify "$A_TH" "$B_TH" 100 100 t)"
  A_TK='❯ 47.0K (84%)'; B_TK='❯ 47.1K (84%)'
  chk "tokens-change-working" "WORKING (" "$(classify "$A_TK" "$B_TK" 100 100 t)"
  chk "cpu-delta-working" "WORKING (" "$(classify "$A_IDLE" "$A_IDLE" 100 130 t)"
  chk "cpu-flat-idle" "IDLE (" "$(classify "$A_IDLE" "$A_IDLE" 100 100 t)"
  chk "modal-blocked" "BLOCKED (" "$(classify 'needs your decision' "$A_IDLE" 0 0 t)"
  A_MK='esc interrupt ❯ x'; 
  chk "markers-esc-working" "WORKING (" "$(classify "$A_MK" "$A_MK" 100 100 t)"
  A_ST='esc interrupt · 23.6s ❯ x'
  chk "markers-frozen-stuck" "STUCK (" "$(classify "$A_ST" "$A_ST" 100 100 t)"
  chk "markers-frozen-cpu-working" "WORKING (" "$(classify "$A_ST" "$A_ST" 100 130 t)"
  chk "norm-keeps-timer" "· 23.6s" "$(printf 'opencode · 23.6s x  \n' | norm)"
  chk "norm-keeps-spinner" "⬝" "$(printf '⬝ working\n' | norm)"
  chk "norm-keeps-thought" "Thought ·" "$(printf 'Thought · 23.6s\n' | norm)"
  chk "norm-strips-ansi" "xy" "$(printf '\x1b[32mxy\x1b[0m  \n\n' | norm)"
  echo "worker-state selftest: $pass/$total PASS"
  [ "$pass" -eq "$total" ]
  exit $?
fi
WIN=20
sess="${1:?usage: worker-state.sh <session>}"
if ! tmux has-session -t "$sess" 2>/dev/null; then echo "$sess ABSENT (no tmux session)"; exit 0; fi
pane=$(tmux list-panes -t "$sess" -F '#{pane_pid}' 2>/dev/null | head -n 1)
[ -n "${pane:-}" ] || { echo "$sess ABSENT (no pane)"; exit 0; }
sid=$(ps -o sid= -p "$pane" 2>/dev/null | tr -d ' ')
snap() { # echoes: pane|timer|tokens|cpu|modalhits
  local p h t tk cc mm
  p=$(tmux capture-pane -t "$sess" -p 2>/dev/null)
  mm=$(printf '%s' "$p" | grep -ciE "$MODAL_RE" || true)
  t=$(printf '%s' "$p" | timer_of)
  tk=$(printf '%s' "$p" | tokens_of)
  cc=$(cpu_total "$sid")
  printf '%s\n@@@%s@@@%s@@@%s@@@%s' "$p" "$t" "$tk" "$cc" "$mm"
}
A=$(snap); sleep "$WIN"; B=$(snap)
pa=${A%%@@@*}; r1=${A#*@@@}; t1=${r1%%@@@*}; r1=${r1#*@@@}; k1=${r1%%@@@*}; r1=${r1#*@@@}; c1=${r1%%@@@*}; m1=${r1##*@@@}
pb=${B%%@@@*}; r2=${B#*@@@}; t2=${r2%%@@@*}; r2=${r2#*@@@}; k2=${r2%%@@@*}; r2=${r2#*@@@}; c2=${r2%%@@@*}; m2=${r2##*@@@}
classify "$pa" "$pb" "$c1" "$c2" "$sess"
