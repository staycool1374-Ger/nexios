#!/usr/bin/env bash
# pilot_watch.sh — T8 comparison collector (issue #507 §5.5).
#
#   pilot_watch.sh <room> --once     append one snapshot block to
#                                  /tmp/nherdr-<room>/reports/pilot-log.md
#   pilot_watch.sh <room> [secs]     same, then every <secs> seconds (default 900)
#
# Each snapshot: phase/state/orders, lane post counts, stream length, herdr
# lane + engine terminal states, plus any operator NEEDs. Used by the T8
# pilot (§5.5 comparison record) and reviewed in the T9 memo.
set -u

ROOM="${1:?need room (e.g. 420)}"
shift || true
SNAP=/tmp/nherdr-"$ROOM"/reports/pilot-log.md

snapshot() {
  local ts state phase orders dev eng lines needs
  ts=$(date -u +%Y-%m-%dT%H:%M:%SZ)
  if [ -f /tmp/nherdr-"$ROOM"/state.json ]; then
    phase=$(python3 -c "import json;print(json.load(open('/tmp/nherdr-$ROOM/state.json')).get('phase'))")
    state=$(python3 -c "import json;print(json.load(open('/tmp/nherdr-$ROOM/state.json')).get('state'))")
    orders=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print(len(d.get('orders',{})))")
    needs=$(python3 -c "import json;d=json.load(open('/tmp/nherdr-$ROOM/state.json'));print(sum(1 for o in d.get('orders',{}).values() if o.get('operator')))")
  else
    phase=? state=? orders=? needs=?
  fi
  dev=$(hc get herdr-pilot-dev --json 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin).get("state"))' 2>/dev/null || echo lost)
  eng=$(hc get herdr-eng-420 --json 2>/dev/null | python3 -c 'import json,sys;print(json.load(sys.stdin).get("state"))' 2>/dev/null || echo lost)
  lines=$(wc -l < /tmp/nherdr-"$ROOM"/posts.jsonl 2>/dev/null || echo 0)
  printf '## %s\nroom=%s phase=%s state=%s orders=%s operator_needs=%s dev=%s engine=%s stream_lines=%s\n' \
    "$ts" "$ROOM" "$phase" "$state" "$orders" "$needs" "$dev" "$eng" "$lines" >> "$SNAP"
}

if [ "${1:-}" = "--once" ]; then snapshot; exit 0; fi
period=${1:-900}
if [ "$period" -lt 60 ]; then period=60; fi
while :; do snapshot; sleep "$period"; done
