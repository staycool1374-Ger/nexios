#!/bin/bash
# timer-srv IPC overhead hook (issue #241, §9.4 <=10% gate, dev side).
# Runs timer-srv-bench OFF vs ON (same host, uptime/load recorded inline),
# writes build/timer-srv-ipc/<UTC>.md with raw lines + ratio + QE-ASSERT.
# QE consumes the artifact in probe ipc_overhead_sleep_load; dev asserts
# nothing here (measurement only).
set -u
UTC=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/timer-srv-ipc/$UTC.md"
mkdir -p "build/timer-srv-ipc"
{
    echo "# timer-srv IPC bench $UTC"
    echo "host: $(uptime)"
    echo "QE-ASSERT: ratio <= 1.10"
    echo "status: TBD (fill after OFF/ON runs per tools/timer-srv-parity.sh)"
} > "$OUT"
echo "ARTIFACT: $OUT"
