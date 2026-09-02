#!/usr/bin/env bash
# run_config_matrix.sh
# ------------------------------------------------------------
# Executes the kernel test‑suite for every *meaningful* configuration
# of the deadline/WCET feature set.
#
# Meaningful combos = 48:
#   CONFIG_DEADLINE_MONITOR_TASK      (0|1)
#   CONFIG_DEADLINE_MISS_DETECTION    (0|1)
#   CONFIG_WCET_OVERRUN_DETECTION    (0|1)
#   CONFIG_SPORADIC_SERVER_DEADLINE_HOOK (0|1)
#   CONFIG_DEADLINE_ACTION (0..4) – only relevant when
#                                  CONFIG_DEADLINE_MISS_DETECTION==1
# ------------------------------------------------------------
set -euo pipefail

# -----------------------------------------------------------------
# Parallelism within a single build (make -j).  Start with CPU cores,
# cap at 6 as suggested by the user (adjustable).
# -----------------------------------------------------------------
JOBS="${JOBS:-6}"
CPU_CORES=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
if (( JOBS > CPU_CORES )); then JOBS=$CPU_CORES; fi

# -----------------------------------------------------------------
# Paths
# -----------------------------------------------------------------
CONFIG_FILE="src/kernel/jarvis_config.h"
CONFIG_BACKUP="${CONFIG_FILE}.matrix_backup"
BUGS_TMP="/tmp/jarvis_config_matrix_bugs.tmp"
SUCCESS_LIST="/tmp/jarvis_config_matrix_success.tmp"

# -----------------------------------------------------------------
# Initialise scratch files
# -----------------------------------------------------------------
> "$BUGS_TMP"
> "$SUCCESS_LIST"

# -----------------------------------------------------------------
# Backup original config (restored on exit / after matrix)
# -----------------------------------------------------------------
cp "$CONFIG_FILE" "$CONFIG_BACKUP"
restore_config() { cp "$CONFIG_BACKUP" "$CONFIG_FILE"; }
trap restore_config EXIT

# -----------------------------------------------------------------
# Helper: set a single #define in jarvis_config.h
# -----------------------------------------------------------------
set_define() {
    local macro="$1" value="$2"
    # BSD sed (macOS) and GNU sed compatible
    sed -i '' "s/^#define ${macro} .*/#define ${macro} ${value}/" "$CONFIG_FILE"
}

# -----------------------------------------------------------------
# Run one configuration combination
# -----------------------------------------------------------------
run_one() {
    local MONITOR=$1 MISS=$2 WCET=$3 SPORADIC=$4 ACTION=$5

    local id="DMON${MONITOR}_DMISS${MISS}_WCET${WCET}_SPO${SPORADIC}_DACT${ACTION}"
    local log="/tmp/jarvis_config_matrix_${id}.log"

    # ---- 1. Re‑configure jarvis_config.h ----
    restore_config
    set_define CONFIG_DEADLINE_MONITOR_TASK           "$MONITOR"
    set_define CONFIG_DEADLINE_MISS_DETECTION         "$MISS"
    set_define CONFIG_WCET_OVERRUN_DETECTION          "$WCET"
    set_define CONFIG_SPORADIC_SERVER_DEADLINE_HOOK   "$SPORADIC"
    set_define CONFIG_DEADLINE_ACTION                 "$ACTION"

    echo "[$id] Building …"

    # ---- 2. Clean build ----
    make clean > /dev/null 2>&1 || true
    make -j"$JOBS" > /dev/null 2>&1 || {
        echo "[$id] BUILD FAILED – skipping test"
        echo "- **$id**: BUILD FAILED – no test log" >> "$BUGS_TMP"
        return
    }

    # ---- 3. Run tests ----
    echo "[$id] Testing …"
    # TEST_SERIAL_LOG is the variable the Makefile's host‑watchdog monitors.
    # Use `||` to avoid `set -e` killing the script when a test fails.
    local status=0
    make execute-test x86_64 debug all TEST_SERIAL_LOG="$log" > /dev/null 2>&1 || status=$?

    # ---- 4. Analyse result ----
    if [ $status -ne 0 ] && grep -q "\[HOST-WATCHDOG\]" "$log" 2>/dev/null; then
        # Watchdog fired → hang
        echo "[$id] HANG"
        echo "- **$id**: HANG – ${log}" >> "$BUGS_TMP"
    elif [ $status -ne 0 ]; then
        # Non‑zero exit but no watchdog → test failure
        echo "[$id] FAIL"
        echo "- **$id**: FAIL – ${log}" >> "$BUGS_TMP"
    else
        # Success – remember for later purge
        echo "[$id] PASS"
        echo "$log" >> "$SUCCESS_LIST"
    fi
}

# -----------------------------------------------------------------
# Generate the 48 meaningful combos
# -----------------------------------------------------------------
TOTAL=48
counter=0

for MONITOR in 0 1; do
  for WCET in 0 1; do
    for SPORADIC in 0 1; do
      # ---- miss detection disabled: action is irrelevant, force 0 ----
      ((counter++))
      echo "[$counter/$TOTAL] DMON${MONITOR}_DMISS0_WCET${WCET}_SPO${SPORADIC}_DACT0"
      run_one "$MONITOR" 0 "$WCET" "$SPORADIC" 0

      # ---- miss detection enabled: iterate actions 0..4 ----
      for ACTION in {0..4}; do
        ((counter++))
        echo "[$counter/$TOTAL] DMON${MONITOR}_DMISS1_WCET${WCET}_SPO${SPORADIC}_DACT${ACTION}"
        run_one "$MONITOR" 1 "$WCET" "$SPORADIC" "$ACTION"
      done
    done
  done
done

# -----------------------------------------------------------------
# 5) Purge successful logs
# -----------------------------------------------------------------
while IFS= read -r logpath; do
    rm -f "$logpath"
done < "$SUCCESS_LIST"

# -----------------------------------------------------------------
# 6) Append bug list to BUGS.md (if non‑empty)
# -----------------------------------------------------------------
if [[ -s "$BUGS_TMP" ]]; then
    {
        echo ""
        echo "## Config‑Matrix Bugs – $(date '+%Y-%m-%d %H:%M:%S')"
        cat "$BUGS_TMP"
        echo ""
    } >> BUGS.md
    echo "Bug list written to BUGS.md"
else
    echo "All $TOTAL configurations passed – no bugs."
fi

echo "=== Config matrix run complete ==="
