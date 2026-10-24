#!/usr/bin/env bash
# run_config_matrix.sh (Meson flow, issue #170)
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
#
# Config mechanism: the five tunables are `#ifndef`-guarded in
# src/kernel/nexios_config.h, so they are overridden per combo with
# `meson configure -Dconfig_defs="..."` on a dedicated matrix builddir —
# the tracked header is never edited (this also fixes the old script's
# KNOWN ISSUE: it sed-edited src/kernel/jarvis_config.h, which does not
# exist). The builddir is shared with tools/deadline_matrix.sh (never run
# concurrently; leftover-QEMU cleanup between runs is owned by
# qemu_test.py:scoped_pkill per attempt, checkout-scoped by tag).
# ------------------------------------------------------------
set -euo pipefail

MATRIX_BUILDDIR="build/meson-x86_64-matrix"
if [ ! -f "$MATRIX_BUILDDIR/build.ninja" ]; then
    meson setup "$MATRIX_BUILDDIR" --cross-file cross/x86_64-elf.txt \
        -Darch=x86_64 -Db_lto=false || exit 2
fi

# -----------------------------------------------------------------
# Paths
# -----------------------------------------------------------------
BUGS_TMP="/tmp/jarvis_config_matrix_bugs.tmp"
SUCCESS_LIST="/tmp/jarvis_config_matrix_success.tmp"
TAG=$(basename "$PWD" | sed 's/[^A-Za-z0-9_]/_/g')
[ -n "$TAG" ] || TAG=checkout
SERIAL=/tmp/jarvis-meson-$TAG/serial.log

# -----------------------------------------------------------------
# Initialise scratch files
# -----------------------------------------------------------------
> "$BUGS_TMP"
> "$SUCCESS_LIST"

# -----------------------------------------------------------------
# Run one configuration combination
# -----------------------------------------------------------------
run_one() {
    local MONITOR=$1 MISS=$2 WCET=$3 SPORADIC=$4 ACTION=$5

    local id="DMON${MONITOR}_DMISS${MISS}_WCET${WCET}_SPO${SPORADIC}_DACT${ACTION}"
    local log="/tmp/jarvis_config_matrix_${id}.log"

    # ---- 1. Re‑configure via the designed config_defs knob ----
    echo "[$id] Building …"
    if ! meson configure "$MATRIX_BUILDDIR" \
            "-Dconfig_defs=-DCONFIG_DEADLINE_MONITOR_TASK=$MONITOR,-DCONFIG_DEADLINE_MISS_DETECTION=$MISS,-DCONFIG_WCET_OVERRUN_DETECTION=$WCET,-DCONFIG_SPORADIC_SERVER_DEADLINE_HOOK=$SPORADIC,-DCONFIG_DEADLINE_ACTION=$ACTION" \
            > /dev/null 2>&1 \
        || ! meson compile -C "$MATRIX_BUILDDIR" > /dev/null 2>&1; then
        echo "[$id] BUILD FAILED – skipping test"
        echo "- **$id**: BUILD FAILED – no test log" >> "$BUGS_TMP"
        return
    fi

    # ---- 2. Run tests ----
    echo "[$id] Testing …"
    # Use `||` to avoid `set -e` killing the script when a test fails.
    # The full `all` class reboots once per class (same as the old flow).
    local status=0
    python3 tools/meson/qemu_test.py --arch x86_64 --build debug \
        --class all --builddir "$MATRIX_BUILDDIR" > /dev/null 2>&1 || status=$?
    # Snapshot the shared serial log into this combo's log before the next
    # run overwrites it.
    cp "$SERIAL" "$log" 2>/dev/null || true

    # ---- 3. Analyse result ----
    if [ $status -ne 0 ] && grep -q "HOST-WATCHDOG" "$log" 2>/dev/null; then
        # Stall monitor fired → hang
        echo "[$id] HANG"
        echo "- **$id**: HANG – ${log}" >> "$BUGS_TMP"
    elif [ $status -ne 0 ]; then
        # Non‑zero exit but no stall → test failure
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
      counter=$((counter + 1))
      echo "[$counter/$TOTAL] DMON${MONITOR}_DMISS0_WCET${WCET}_SPO${SPORADIC}_DACT0"
      run_one "$MONITOR" 0 "$WCET" "$SPORADIC" 0

      # ---- miss detection enabled: iterate actions 0..4 ----
      for ACTION in {0..4}; do
        counter=$((counter + 1))
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
