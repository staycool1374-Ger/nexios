#!/bin/bash
# term-srv loopback/parity harness (issue #51, strangler step b).
# Captures `S TM` lines on OFF (default) + ON ISOs via the proven
# tools/srv-pd-parity.py harness (QEMU monitor sendkey + file-backed
# COM1: raw expect-on-spawn observes neither file-backed serial nor
# guest input), diffs the filtered sets (must be byte-identical:
# emulation policy is unchanged pre-flip; only the PD exists).
# Usage: tools/term-srv-parity.sh <off-iso> <on-iso> [fat32.img]
# Artifacts: build/term-srv-parity/<UTC>/ (serial + filtered + diff).
# NOTE: both ISOs must bake test_class=none (interactive shell).
set -u
OFF_ISO="${1:?usage: term-srv-parity.sh <off-iso> <on-iso> [fat32.img]}"
ON_ISO="${2:?usage: term-srv-parity.sh <off-iso> <on-iso> [fat32.img]}"
FAT="${3:-build/meson-x86_64/packaging/fat32.img}"
UTC=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/term-srv-parity/$UTC"
mkdir -p "$OUT"
echo "term-srv PARITY $UTC (OFF default vs ON shim)"
PORT=4490
for variant in OFF ON; do
    ISO_VAR="${variant}_ISO"
    PORT=$((PORT + 1))
    python3 tools/srv-pd-parity.py "${!ISO_VAR}" /bin/term-srv-parity.c.elf "S TM " \
        "$OUT/$variant" --fat32 "$FAT" --port "$PORT" || exit 1
    cp "$OUT/$variant/filtered.txt" "$OUT/filtered-$variant.txt"
    cp "$OUT/$variant/com1.log" "$OUT/serial-$variant.log"
done
if diff -u "$OUT/filtered-OFF.txt" "$OUT/filtered-ON.txt" > "$OUT/diff.txt"; then
    echo "PARITY-OK: OFF == ON (see $OUT)"
else
    echo "PARITY-DIFF: see $OUT/diff.txt"
    exit 1
fi
