#!/bin/bash
# timer-srv loopback/parity harness (issue #241, strangler step b).
# Builds OFF (default) + ON (-DCONFIG_TIMER_SRV=1) ISOs, captures
# `S TMR` lines via timer-srv-parity.exp, diffs the filtered sets.
# Usage: tools/timer-srv-parity.sh   (artifacts: build/timer-srv-parity/<UTC>/)
set -u
UTC=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/timer-srv-parity/$UTC"
mkdir -p "$OUT"
echo "timer-srv PARITY $UTC (OFF default vs ON shim)"
echo "NOTE: pre-flip the server implements BIND/CLOCK_GETTIME/CANCEL only;"
echo "full-op parity is a QE T1 gate after the flip, not this script."
echo "ARTIFACT-DIR: $OUT"
