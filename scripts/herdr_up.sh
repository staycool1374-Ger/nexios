#!/usr/bin/env bash
# herdr_up.sh — setup/driver for the herdr-core (0.1.18) engine pilot.
# Pilot namespace: /tmp/nherdr-<issue>/ (separate from legacy /tmp/nexios-<issue>/).
# Verbs (T0 skeleton: -h + selftest; T1 adds setup/testbed-rooms/status):
#   selftest [--full]  run the T0 spawn-proof sub-check (12 fast checks; --full
#                      adds the 3 opencode model-dependent modal checks)
# fence doctrine (issue #507 §4): every engineered spawn is
#   hc run --bg --name <t> --cwd <dir> env OPENCODE_CONFIG=<rendered role fence>
#     TMPDIR=/tmp/agentup-tmp-<terminal> NEXIOS_AGENT_ROLE=<role> <OPENCODE_BIN> ...
# and identity is verified from the child process environment before the
# engine declares the terminal usable; mismatch => kill on sight.
# Platform note (verified T0): macOS hides `ps eww` environ for Apple-signed
# programs (zsh/sh/sleep); unsigned programs (opencode, homebrew python3)
# expose it. Identity therefore reads the opencode/agent process env — same
# seam as legacy check_fenced, which also scans for the opencode binary.

set -u

NEXIOS_HOME="${NEXIOS_HOME:-$HOME/.local/state/nexios}"
OPENCODE_BIN="${OPENCODE_BIN:-$HOME/.opencode/bin/opencode}"
HC_BIN="${HC_BIN:-$HOME/.local/bin/hc}"
PYTHON="${PYTHON:-/opt/homebrew/bin/python3}"
TAG="t0s-$RANDOM"  # unique terminal name prefix per run

pass=0
fail=0
declare -a FAILED=()

note() { printf '%s: %s\n' "$1" "$2"; }
ok()   { pass=$((pass + 1)); note PASS "$1"; }
bad()  { fail=$((fail + 1)); FAILED+=("$1"); note FAIL "$1"; }

check_env_value() { # $1 pid, $2 key=value token
  local env; env="$(ps eww -p "$1" 2>/dev/null)"
  case "$env" in *"$2"*) return 0 ;; *) return 1 ;; esac
}

foreground_pid() { # $1 hc terminal name -> pid of its foreground program
  "$HC_BIN" get "$1" --json 2>/dev/null \
    | "$PYTHON" -c 'import json,sys; d=json.load(sys.stdin); f=d.get("foreground") or {}; print(f.get("pid") or "")'
}

field() { # $1 hc terminal name, $2 json field (state|exit_code|...)
  "$HC_BIN" get "$1" --json 2>/dev/null | "$PYTHON" -c "import json,sys; print(json.load(sys.stdin).get('$2'))"
}

# verify_identity <hc-name> <want-pwd> <want-cfg> <want-role> <want-tmpdir>
# requires all four fence values on the terminal's foreground program env.
verify_identity() {
  local name="$1" want_pwd="$2" want_cfg="$3" want_role="$4" want_tmp="$5"
  local fpid
  fpid=$(foreground_pid "$name") || return 1
  [ -n "$fpid" ] || return 1
  check_env_value "$fpid" "PWD=$want_pwd"      || return 1
  check_env_value "$fpid" "OPENCODE_CONFIG=$want_cfg" || return 1
  check_env_value "$fpid" "NEXIOS_AGENT_ROLE=$want_role" || return 1
  check_env_value "$fpid" "TMPDIR=$want_tmp"   || return 1
  return 0
}

tidy() { # clean up our throwaway terminals
  local t
  for t in $("$HC_BIN" ls --json 2>/dev/null \
      | "$PYTHON" -c 'import json,sys
for t in json.load(sys.stdin)["terminals"]:
    if t["name"].startswith("'"$TAG"'"):
        print(t["name"])' 2>/dev/null); do
    "$HC_BIN" kill "$t" >/dev/null 2>&1
    "$HC_BIN" rm "$t" >/dev/null 2>&1
  done
}

trap 'tidy' EXIT

selftest() {
  local full=0
  [ "${1:-}" = "--full" ] && full=1

  local sandbox=/tmp/nherdr-selftest-$$
  local cwd="$sandbox/cwd"
  mkdir -p "$cwd"
  local dummy_cfg="$sandbox/opencode.dummy.json"
  printf '{"$schema":"https://opencode.ai/config.json"}' > "$dummy_cfg"
  local ask_cfg="$sandbox/opencode.ask.json"
  printf '{"$schema":"https://opencode.ai/config.json","permission":{"edit":"ask","bash":"ask","webfetch":"ask"}}' > "$ask_cfg"

  local cfg="$dummy_cfg" role=dev i state kv wait_json
  local name_sh="$TAG-sh"  # zsh terminal: process-behaviour checks
  local name_py="$TAG-py"  # python3 terminal: env identity checks (unsigned exec)
  local name_ev="$TAG-ev"

  # -- Phase A: zsh terminal, process-behaviour checks ------------------

  # C1 — spawn: fenced shell terminal comes up live under hc run --bg
  "$HC_BIN" run --bg --name "$name_sh" --cwd "$cwd" \
    env TMPDIR="/tmp/agentup-tmp-$name_sh" OPENCODE_CONFIG="$cfg" \
    NEXIOS_AGENT_ROLE="$role" zsh >/dev/null 2>&1
  state=$(field "$name_sh" state)
  [ "$state" = "live" ] && ok "C1 spawn: terminal state live under hc run --bg" \
                           || bad "C1 spawn: terminal state '$state' != live"

  # C2 — child runs in the requested cwd (marker file written from the child)
  "$HC_BIN" send "$name_sh" ": > '$cwd/.marker'" >/dev/null 2>&1
  local marker_ok=0
  for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -e "$cwd/.marker" ] && { marker_ok=1; break; }
    sleep 0.5
  done
  [ "$marker_ok" = 1 ] && ok "C2 cwd inherit: child runs in --cwd directory" \
                          || bad "C2 cwd inherit: marker never appeared in $cwd"

  # -- Phase B: python3 terminal (unsigned -> env readable), identity checks --

  "$HC_BIN" run --bg --name "$name_py" --cwd "$cwd" \
    env TMPDIR="/tmp/agentup-tmp-$name_py" OPENCODE_CONFIG="$cfg" \
    NEXIOS_AGENT_ROLE="$role" "$PYTHON" -q >/dev/null 2>&1
  sleep 1

  # C3..C6 — all four fence values present in the child process env
  local fpid
  fpid=$(foreground_pid "$name_py")
  for kv in "PWD=$cwd" "OPENCODE_CONFIG=$cfg" "NEXIOS_AGENT_ROLE=$role" "TMPDIR=/tmp/agentup-tmp-$name_py"; do
    if check_env_value "$fpid" "$kv"; then
      ok "fence env: $kv present in child process"
    else
      bad "fence env: $kv MISSING in child process"
    fi
  done

  # C7 — identity seam: four-value verify passes on the matching spawn
  if verify_identity "$name_py" "$cwd" "$cfg" "$role" "/tmp/agentup-tmp-$name_py"; then
    ok "C7 identity: verify_identity passes the correctly fenced spawn"
  else
    bad "C7 identity: verify_identity failed on a correctly fenced spawn"
  fi

  # C8 — mismatch detection: role-mutated spawn fails the same seam
  local name_unk="$TAG-unk"
  "$HC_BIN" run --bg --name "$name_unk" --cwd "$cwd" \
    env TMPDIR="/tmp/agentup-tmp-$name_unk" OPENCODE_CONFIG="$cfg" \
    NEXIOS_AGENT_ROLE=qe "$PYTHON" -q >/dev/null 2>&1   # fence says dev, bring-up says qe
  if verify_identity "$name_unk" "$cwd" "$cfg" "dev" "/tmp/agentup-tmp-$name_unk"; then
    bad "C8 mismatch: verify_identity wrongly passes role=qe spawn for role dev"
  else
    ok "C8 mismatch: verify_identity rejects role-mutated spawn"
  fi

  # C9 — kill on mismatch: hc kill ends the flagged terminal for good
  "$HC_BIN" kill "$name_unk" >/dev/null 2>&1
  state=""
  for i in 1 2 3 4 5; do
    state=$(field "$name_unk" state)
    [ "$state" = "finished" ] && break
    sleep 0.5
  done
  [ "$state" = "finished" ] && ok "C9 kill-on-mismatch: hc kill => state finished" \
                              || bad "C9 kill-on-mismatch: state '$state' != finished"

  # -- Phase C: herdr event/wait surface ---------------------------------

  # C10 — events shaping: created + finished (killed) land on the event bus
  ( "$HC_BIN" events --json 2>/dev/null > "$sandbox/events.out" & echo $! > "$sandbox/events.pid" )
  "$HC_BIN" run --bg --name "$name_ev" --cwd "$cwd" env TMPDIR="/tmp/agentup-tmp-$name_ev" zsh >/dev/null 2>&1
  "$HC_BIN" kill "$name_ev" >/dev/null 2>&1
  local evpid created=0 finished=0
  evpid=$(cat "$sandbox/events.pid")
  for i in 1 2 3 4 5 6; do
    grep -q '"type":"created"'  "$sandbox/events.out" && created=1
    grep -q '"type":"finished"' "$sandbox/events.out" && finished=1
    [ $created = 1 ] && [ $finished = 1 ] && break
    sleep 0.5
  done
  kill "$evpid" 2>/dev/null
  [ $created = 1 ] && [ $finished = 1 ] \
    && ok "C10 events: created + finished transitions visible on hc events" \
    || bad "C10 events: transitions not visible (created=$created finished=$finished)"

  # C11 — wait shaping: sentinel text echoed to the screen ends wait --output.
  # --output only sees text printed after the wait started, so the wait races
  # ahead of the send deliberately (structured like the modal check).
  ( "$HC_BIN" wait "$name_sh" --json --timeout 30 --output done-wait-shape > "$sandbox/wait.out" 2>&1 ) &
  local waitpid=$!
  sleep 1
  "$HC_BIN" send "$name_sh" "echo done-wait-shape" >/dev/null 2>&1
  wait "$waitpid" 2>/dev/null
  wait_json=$(cat "$sandbox/wait.out")
  local rc_out=0
  case "$wait_json" in *'"ended":true'*'"reason":"output"'*) rc_out=1 ;; esac
  [ "$rc_out" = 1 ] && ok "C11 wait: wait --output ends when sentinel text is printed" \
                      || bad "C11 wait: wait --output did not end (got: $wait_json)"

  # C12 — false-positive control: wait times out when nothing matches
  wait_json=$("$HC_BIN" wait "$name_sh" --json --timeout 3 --output 'UNMATCHABLE-XX' 2>/dev/null)
  case "$wait_json" in *'"ended":false'*'"reason":"timeout"'*) \
    ok "C12 wait control: unmatching pattern times out (no false positive)" ;; *) \
    bad "C12 wait control: unexpected result $wait_json" ;; esac

  # -- Phase D: opencode-model-dependent, opt-in via --full ----------------

  if [ "$full" = 1 ]; then
    opencode_full_checks "$cwd" "$sandbox"
  else
    note SKIP "C13-C15 opencode model-dependent checks (rerun with --full)"
  fi

  printf '\nselftest spawn-proof: %d passed, %d failed\n' "$pass" "$fail"
  [ $fail -gt 0 ] && printf 'failed: %s\n' "${FAILED[*]}"
  [ $fail = 0 ]
}

opencode_full_checks() { # $1 cwd, $2 sandbox
  local cwd="$1" sandbox="$2"
  local dev_cfg="$NEXIOS_HOME/rendered/dev/opencode.resolved.json"
  local name_oe="$TAG-oe" name_mdl="$TAG-mdl" name_ctl="$TAG-ctl"
  local model="${HC_SELFTEST_MODEL:-opencode/glm-5.3-flash-free}"
  local wait_json modal=0

  # C13 — real opencode under the fence: four-value identity passes
  "$HC_BIN" run --bg --name "$name_oe" --cwd "$cwd" \
    env TMPDIR="/tmp/agentup-tmp-$name_oe" OPENCODE_CONFIG="$dev_cfg" \
    NEXIOS_AGENT_ROLE=dev "$OPENCODE_BIN" -m "$model" >/dev/null 2>&1
  sleep 4
  if verify_identity "$name_oe" "$cwd" "$dev_cfg" dev "/tmp/agentup-tmp-$name_oe"; then
    ok "C13 opencode fence: four-value identity verified on opencode process tree"
  else
    bad "C13 opencode fence: identity verification failed"
  fi

  # C14 — blocked true-positive: permission modal is seen by wait --output.
  # Start the waiter before the task: --output only sees text printed after it.
  "$HC_BIN" run --bg --name "$name_mdl" --cwd "$cwd" \
    env TMPDIR="/tmp/agentup-tmp-$name_mdl" OPENCODE_CONFIG="$ask_cfg" \
    NEXIOS_AGENT_ROLE=dev "$OPENCODE_BIN" -m "$model" >/dev/null 2>&1
  sleep 4
  state=$(field "$name_mdl" state)
  if [ "$state" != "live" ]; then
    bad "C14 setup: modal-terminal state '$state' != live (spawn failed)"; return
  fi
  ( "$HC_BIN" wait "$name_mdl" --json --timeout 120 --output 'Permission required' > "$sandbox/modal.out" 2>&1 ) &
  local modalpid=$!
  sleep 1
  "$HC_BIN" send "$name_mdl" "create a file $sandbox/blocked-proof.txt containing the word hi" >/dev/null 2>&1
  wait "$modalpid" 2>/dev/null
  wait_json=$(cat "$sandbox/modal.out")
  local screen; screen=$("$HC_BIN" read "$name_mdl" 2>/dev/null | grep -v '^\s*$' | tail -6)
  case "$wait_json" in *'"ended":true'*'"reason":"output"'*) modal=1 ;; esac
  if [ $modal = 1 ]; then
    ok "C14 blocked true-positive: wait --output caught the permission modal"
  elif printf '%s' "$screen" | grep -q 'Free usage exceeded\|retrying in'; then
    note SKIP "C14 blocked true-positive: model quota exhausted (rerun when quota returns; manually proven 2026-11-07 — see docs/engine-herdr/spawn-proof.md)"
  else
    bad "C14 blocked true-positive: modal not caught (got: $wait_json)"
  fi

  # C15 — true-negative control: in-cwd write under the real dev fence, no modal
  "$HC_BIN" run --bg --name "$name_ctl" --cwd "$cwd" \
    env TMPDIR="/tmp/agentup-tmp-$name_ctl" OPENCODE_CONFIG="$dev_cfg" \
    NEXIOS_AGENT_ROLE=dev "$OPENCODE_BIN" -m "$model" >/dev/null 2>&1
  sleep 4
  ( "$HC_BIN" wait "$name_ctl" --json --timeout 90 --output 'Permission required' > "$sandbox/ctl.out" 2>&1 ) &
  local ctlpid=$!
  sleep 1
  "$HC_BIN" send "$name_ctl" "create a file hello.txt in this directory (cwd) containing the word ok" >/dev/null 2>&1
  wait "$ctlpid" 2>/dev/null
  wait_json=$(cat "$sandbox/ctl.out")
  local ctl_screen; ctl_screen=$("$HC_BIN" read "$name_ctl" 2>/dev/null | grep -v '^\s*$' | tail -6)
  case "$wait_json" in *'"ended":true'*'"reason":"output"'*) \
    bad "C15 control: modal fired where none was expected" ;; *) \
    ok "C15 control: no modal under the real dev fence for in-cwd write" ;; esac
  if printf '%s' "$ctl_screen" | grep -q 'Free usage exceeded\|retrying in'; then
    note SKIP "C15 file: model quota exhausted (rerun when quota returns)"
    return
  fi
  local ctl_ok=0
  for i in 1 2 3 4 5 6 7 8 9 10; do
    [ -e "$cwd/hello.txt" ] && { ctl_ok=1; break; }
    sleep 1
  done
  [ $ctl_ok = 1 ] && ok "C15 file: in-cwd write produced hello.txt" \
                  || bad "C15 file: hello.txt not produced"
}

usage() {
  cat <<'EOF'
Usage: herdr_up.sh selftest [--full]
  selftest          T0 spawn-proof sub-check (12 fast checks) — no model needed
  selftest --full   adds the 3 opencode checks (fence identity, blocked
                    true-positive on a permission modal, true-negative control)
  -h | help         this text
Requires: hc (herdr-core 0.1.18), homebrew python3, ps. The selftest creates
and kills throwaway hc terminals tagged t0s-*.
EOF
}

case "${1:-help}" in
  selftest) shift || true; selftest "$@" ;;
  -h|help|--help|'') usage ;;
  *) usage; exit 2 ;;
esac
