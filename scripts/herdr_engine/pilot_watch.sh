#!/usr/bin/env bash
# pilot_watch.sh — T8 comparison collector (issue #507 §5.5).
#
#   pilot_watch.sh [room] --once    append one snapshot block per room to
#                                   /tmp/nherdr-<room>/reports/pilot-log.md
#   pilot_watch.sh [room] [secs]    same, then every <secs> seconds (default 300)
#
# With no room arg, watches every /tmp/nherdr-<digits> room. Each snapshot:
# phase/state/orders/open orders/BLOCKED handovers, stream length, herdr
# lane + engine terminal states, legacy operator NEED flags (pre-durable-
# queue rooms only — no new order ever carries one), and stall flags.
# Used by the T8 pilot (§5.5 comparison record) and reviewed in the T9 memo.
set -u

room_arg="${1:-}"
once=0
if [ "$room_arg" = "--once" ]; then room_arg=""; once=1; shift || true
elif [ -n "$room_arg" ] && [ "$room_arg" != "--once" ]; then shift || true; fi

snapshot_room() {
  local ROOM="$1"
  local SNAP=/tmp/nherdr-"$ROOM"/reports/pilot-log.md
  local ts state phase orders open needs stalled blocked
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  if [ -f /tmp/nherdr-"$ROOM"/state.json ]; then
    phase=$(python3 -c "import json;print(json.load(open('/tmp/nherdr-$ROOM/state.json')).get('phase'))")
    state=$(python3 -c "import json;print(json.load(open('/tmp/nherdr-$ROOM/state.json')).get('state'))")
    orders=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print(len(d.get('orders',{})))")
    open=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print(sum(1 for o in d.get('orders',{}).values() if o.get('state')=='open'))")
    blocked=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print(sum(1 for o in d.get('orders',{}).values() if o.get('state')=='blocked'))")
    needs=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print(sum(1 for o in d.get('orders',{}).values() if o.get('operator') or o.get('operator_needed')))")
    stalled=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print('stalled' if d.get('stalled') else 'ok')")
    mkdir -p /tmp/nherdr-"$ROOM"/reports
  else
    phase=? state=? orders=? open=? needs=? stalled=? blocked=?
  fi
  if [ -f "/tmp/nherdr-$ROOM/posts.jsonl" ]; then
    lines=$(wc -l < "/tmp/nherdr-$ROOM/posts.jsonl" | tr -d ' ')
  else
    lines=0
  fi
  printf '## %s\nroom=%s phase=%s state=%s orders=%s open=%s blocked=%s operator_needs=%s stalled=%s stream_lines=%s\n' \
    "$ts" "$ROOM" "$phase" "$state" "$orders" "$open" "$blocked" "$needs" "$stalled" "$lines" >> "$SNAP"
}

snapshot_all() {
  local d r
  for d in /tmp/nherdr-[0-9]*; do
    [ -d "$d" ] || continue
    r=${d#/tmp/nherdr-}
    case "$r" in ''|*[!0-9]*) continue ;; esac
    if [ -n "$room_arg" ] && [ "$r" != "$room_arg" ]; then continue; fi
    snapshot_room "$r"
  done
  local dev eng
  dev=$(hc get herdr-pilot-dev --json 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin).get("state"))' 2>/dev/null || echo lost)
  eng=$(hc get herdr-eng-420 --json 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin).get("state"))' 2>/dev/null || echo lost)
  printf 'lanes: dev=%s engine=%s\n' "$dev" "$eng" >> "/tmp/nherdr-${room_arg:-420}/reports/pilot-log.md"
}

if [ "$once" = 1 ] || [ "${1:-}" = "--once" ]; then snapshot_all; exit 0; fi
period=${1:-300}
if [ "$period" -lt 60 ]; then period=60; fi
while :; do snapshot_all; sleep "$period"; done
