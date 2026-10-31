#!/bin/bash
# term-srv loopback/parity harness (issue #51, strangler step b).
# Builds OFF (default) + ON ISOs, captures `S TM` lines via
# term-srv-parity.exp, diffs the filtered sets (must be byte-identical:
# emulation policy is unchanged pre-flip; only the PD exists).
# Usage: tools/term-srv-parity.sh <off-iso> <on-iso>
# Artifacts: build/term-srv-parity/<UTC>/ (serial + filtered + diff).
set -u
OFF_ISO="${1:?usage: term-srv-parity.sh <off-iso> <on-iso>}"
ON_ISO="${2:?usage: term-srv-parity.sh <off-iso> <on-iso>}"
UTC=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/term-srv-parity/$UTC"
mkdir -p "$OUT"
echo "term-srv PARITY $UTC (OFF default vs ON shim)"
for variant in OFF ON; do
    ISO_VAR="${variant}_ISO"
    tools/term-srv-parity.exp "${!ISO_VAR}" "$OUT/serial-$variant.log" "$OUT/filtered-$variant.txt"
done
if diff -u "$OUT/filtered-OFF.txt" "$OUT/filtered-ON.txt" > "$OUT/diff.txt"; then
    echo "PARITY-OK: OFF == ON (see $OUT)"
else
    echo "PARITY-DIFF: see $OUT/diff.txt"
    exit 1
fi
