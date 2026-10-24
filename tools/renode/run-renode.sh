#!/usr/bin/env bash
# ==============================================================================
# Jarvis RTOS — Renode Launcher
#
# Usage:
#   ./tools/renode/run-renode.sh                    Run x86_64 in Renode (default)
#   ./tools/renode/run-renode.sh x86_64             Run x86_64 in Renode
#   ./tools/renode/run-renode.sh aarch64            Run AArch64 in Renode
#   ./tools/renode/run-renode.sh riscv64            Run RISC-V 64 in Renode
#   ./tools/renode/run-renode.sh build              Build debug ISO first
#   ./tools/renode/run-renode.sh test               Run selftest in Renode
# ==============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
WORKSPACE="$(cd "$SCRIPT_DIR/../.." && pwd)"
ARCH="${1:-x86_64}"

case "$ARCH" in
    build)
        echo "Building debug ISO..."
        cd "$WORKSPACE"
        tools/meson/build.sh build/meson-x86_64
        echo "Done. Run without 'build' to launch Renode."
        exit 0
        ;;
    test)
        echo "Building debug ISO..."
        cd "$WORKSPACE"
        tools/meson/build.sh build/meson-x86_64
        echo "Running Renode selftest..."
        renode --disable-xwt -e "i @tools/renode/jarvis-x86_64.resc; s;
            sysbus.cpu LOG_LEVEL INFO;
            echo 'Starting Renode simulation...'
            start;
            wait-for-output \"TEST SUMMARY\" 120;
            emit-test-results;
            quit"
        exit 0
        ;;
    x86_64|aarch64|riscv64)
        ;;
    *)
        echo "Usage: $0 [x86_64|aarch64|riscv64|build|test]"
        exit 1
        ;;
esac

RESC="$SCRIPT_DIR/jarvis-${ARCH}.resc"
if [ ! -f "$RESC" ]; then
    echo "ERROR: No boot script for arch '${ARCH}': ${RESC}"
    exit 1
fi

echo "Launching Renode — Jarvis RTOS (${ARCH})"
echo "  Script: ${RESC}"
echo "  ISO:    ${WORKSPACE}/debug/nexios-rtos.iso"
echo ""
echo "Renode commands:"
echo "  start          - start simulation"
echo "  pause          - pause simulation"
echo "  quit           - exit Renode"
echo "  sysbus.cpu LOG_LEVEL INFO - enable CPU logging"
echo "  $name?=        - machine name prefix"
echo ""

renode --disable-xwt -e "i @${RESC}; s" 2>&1
