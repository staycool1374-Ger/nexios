#!/usr/bin/env bash
#
# deadline_matrix.sh — config-matrix runner for Phase 7 (P7a) deadline-miss
# handler ACTION dispatch coverage.
#
# The deadline-miss handler action is selected at COMPILE TIME by
# CONFIG_DEADLINE_ACTION. This script builds the kernel once per action value
# (0=LOG_ONLY, 1=PANIC, 2=DEMOTE, 3=KILL, 4=NOTIFY_MONITOR) and runs the
# `deadline_action` test class for each. PANIC is an expected-fail: the kernel
# must halt, and we treat the presence of the "action=PANIC" message as the
# success signal.
#
# Usage: tools/deadline_matrix.sh
set -u
cd "$(dirname "$0")/.."

ARCH=x86_64
BUILD=debug
# Per-checkout tag (issue #372): same basename+sed transform as the Makefile
# CHECKOUT_TAG so this script watches the SAME namespaced serial log its
# inner `make execute-test` writes, and its pkill only matches this
# checkout's QEMU (prefix trap: match requires a trailing non-identifier
# char so tag `jarvis` never matches `jarvis_test` artifacts).
TAG=$(basename "$PWD" | sed 's/[^A-Za-z0-9_]/_/g')
[ -n "$TAG" ] || TAG=checkout
SERIAL=/tmp/jarvis-serial-$TAG.log
MATRIX_BUILD_LOG=/tmp/jarvis-matrix-build-$TAG.log
MATRIX_RUN_LOG=/tmp/jarvis-matrix-run-$TAG.log
PASS=0
FAIL=0
MATRIX_FAILS=""

run_action() {
    local action="$1"
    local expect_panic="$2"
    echo "=================================================="
    echo "[MATRIX] ACTION=$action  expect_panic=$expect_panic"
    echo "=================================================="

    # Kill any leftover QEMU from a previous (e.g. killed PANIC) run so the
    # next boot starts clean and does not stall on a busy port/lock.
    pkill -9 -f "qemu-system-x86_64.*-name jarvis-$TAG([^A-Za-z0-9_]|$)" 2>/dev/null || true
    sleep 1

    make clean >/dev/null 2>&1
    if ! make "$BUILD" CONFIG_DEFS="-DCONFIG_DEADLINE_ACTION=$action" NO_LTO=1 \
            >"$MATRIX_BUILD_LOG" 2>&1; then
        echo "[MATRIX]   BUILD FAILED — see $MATRIX_BUILD_LOG"
        FAIL=$((FAIL + 1))
        MATRIX_FAILS="$MATRIX_FAILS action$action(build)"
        return
    fi

    # Cap the QEMU run; the PANIC case halts the kernel and the host-side
    # watchdog eventually kills QEMU, so this must not block indefinitely.
    timeout 180 make execute-test "$ARCH" "$BUILD" deadline_action NO_LTO=1 \
            >"$MATRIX_RUN_LOG" 2>&1
    # Let the tee'd serial log fully flush before we grep it.
    sync; sleep 1
    cp "$SERIAL" "/tmp/matrix-action$action-$TAG.log" 2>/dev/null || true

    if [ "$expect_panic" = "1" ]; then
        if grep -q "action=PANIC" "$SERIAL"; then
            echo "[MATRIX]   PANIC observed (expected) -> PASS"
            PASS=$((PASS + 1))
        else
            echo "[MATRIX]   PANIC NOT observed -> FAIL"
            FAIL=$((FAIL + 1))
            MATRIX_FAILS="$MATRIX_FAILS action$action(panic)"
        fi
    else
        # Assert on the per-test PASS marker (robust against summary-flush
        # races) and confirm no FAIL marker for this class.
        if grep -Eq "deadline_action .*: PASS" "$SERIAL" && \
           ! grep -Eq "deadline_action .*: FAIL" "$SERIAL"; then
            echo "[MATRIX]   test PASS -> PASS"
            PASS=$((PASS + 1))
        else
            echo "[MATRIX]   test not PASS -> FAIL"
            FAIL=$((FAIL + 1))
            MATRIX_FAILS="$MATRIX_FAILS action$action"
        fi
    fi
}

run_action 0 0
run_action 1 1
run_action 2 0
run_action 3 0
run_action 4 0

echo "=================================================="
echo "[MATRIX] RESULT: PASS=$PASS FAIL=$FAIL"
if [ -n "$MATRIX_FAILS" ]; then
    echo "[MATRIX] FAILED: $MATRIX_FAILS"
fi
echo "=================================================="
[ "$FAIL" -eq 0 ]
