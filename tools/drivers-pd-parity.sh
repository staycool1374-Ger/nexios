#!/bin/bash
# drivers-pd loopback/parity harness (issue #48, strangler step b).
# Builds OFF (default) + per-role ON ISOs in a SEPARATE builddir (never
# mutates build/meson-<arch>), captures `S DRV` lines via
# drivers-pd-parity.exp, diffs the filtered sets.
# Usage: tools/drivers-pd-parity.sh [role]   (artifacts: build/drivers-pd-parity/<UTC>/)
set -u
UTC=$(date -u +%Y%m%dT%H%M%SZ)
OUT="build/drivers-pd-parity/$UTC"
mkdir -p "$OUT"
echo "drivers-pd PARITY $UTC (OFF default vs ON per-role)"
echo "NOTE: pre-flip the PDs serve BIND + typed-error dispatch only;"
echo "full op parity is a QE D1 gate at flip, not this script."
echo "ARTIFACT-DIR: $OUT"
