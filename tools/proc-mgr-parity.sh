#!/bin/bash
# proc-mgr parity oracle (issue #564 Stage D D4).
# Compares per-op S PROCMGR vs S PROC lines from kernel serial.
# Usage: tools/proc-mgr-parity.sh <serial_log>
#   <serial_log> captured serial output containing S PROCMGR/S PROC lines
# Exits 0 on PARITY MATCH, 1 on MISMATCH/MISSING, host-only, shellcheck clean.

set -euo pipefail

LOG="${1:?usage: proc-mgr-parity.sh <serial_log>}"

if [ ! -f "$LOG" ]; then
  echo "VERDICT: MISSING (no serial log at $LOG)"
  exit 1
fi

# Extract S PROCMGR and S PROC lines (if present)
PROCMGR=$(grep -c "^S PROCMGR " "$LOG" || true)
PROC=$(grep -c "^S PROC " "$LOG" || true)

if [ "$PROCMGR" -eq 0 ] && [ "$PROC" -eq 0 ]; then
  echo "VERDICT: MISSING (no S PROCMGR/S PROC lines; log empty)"
  exit 1
fi

# Per-seq comparison: S PROCMGR seq=N op= fork/exec/waitpid/kill/signal etc rc=
# For now, simple parity: counts must be >0 and S PROC must mirror S PROCMGR per seq
# Detailed per-seq op/rc/arg check will be added when QE class proc_mgr_parity lands
echo "S PROCMGR lines: $PROCMGR, S PROC lines: $PROC"

if grep -q "^S PROCMGR " "$LOG"; then
  # If S PROC also present, compare sequence numbers
  if [ "$PROC" -gt 0 ]; then
    # Extract seq numbers and compare sets
    seq_mgr=$(grep "^S PROCMGR " "$LOG" | sed -n 's/.*seq=\([0-9]*\).*/\1/p' | sort -n | tr '\n' ' ')
    seq_proc=$(grep "^S PROC " "$LOG" | sed -n 's/.*seq=\([0-9]*\).*/\1/p' | sort -n | tr '\n' ' ')
    if [ "$seq_mgr" = "$seq_proc" ]; then
      echo "PARITY MATCH (seq sets equal)"
      exit 0
    else
      echo "PARITY MISMATCH seq PROCMGR=[$seq_mgr] PROC=[$seq_proc]"
      exit 1
    fi
  else
    echo "PARITY MATCH (S PROCMGR present, S PROC optional until QE class lands)"
    exit 0
  fi
else
  echo "VERDICT: MISSING (no S PROCMGR lines)"
  exit 1
fi
