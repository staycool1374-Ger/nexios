#!/bin/sh
# Meson build entry point (issue #170 Phase 6): `make build` equivalent.
#
# Usage: tools/meson/build.sh [builddir] [--help]
#   - Runs tools/validate_style.py first (Errors>0 fails the gate; warnings pass).
#   - Wipes + reconfigures the builddir on .lto-mode/buildtype mismatch
#     (A2: never link mixed LTO objects; same rule as qemu_test.py).
#   - Then `meson compile` (which regenerates the backend if needed).
# With --help, prints the target table (mirrors `make help` scope).

set -u

ROOT="$(dirname "$0")/../.."
BUILD_DIR="${1:-build/meson-x86_64}"
if [ "${1:-}" = "--help" ] || [ "${2:-}" = "--help" ]; then
    cat <<'EOF'
NexIOS Meson build (issue #170). Canonical commands:
  meson setup build/meson-<arch> --cross-file cross/<arch>-elf.txt -Darch=<arch>
  tools/meson/build.sh [builddir]                 # style gate + compile (this script)
  meson compile -C <builddir> [target]            # rebuild (all targets incl. ISO, registry)
  meson test -C <builddir> --suite <arch>-debug   # QEMU suites (26 classes, use --no-rebuild after warm build)
  meson test -C <builddir> --suite host           # host parity tests (flags, registry, counts, policy, verdicts)
  meson test -C <builddir> --suite host test_flag_parity   # single host test
  meson test -C <builddir> 'qemu/x86_64/debug/<class>'     # single QEMU class
  meson configure <builddir> -Dtest_class=<class> # switch baked class (relinks on next build)
  meson configure <builddir> -Db_lto=false         # NO_LTO variant (full rebuild, never mix)
  python3 tools/meson/qemu_test.py --arch A --build B --class C   # single class, explicit
  python3 tools/meson/qemu_test.py --arch A --build B --dump-counts  # TCOUNT dump
  python3 tools/meson/gdb_run.py debug-test ...    # GDB panic surveillance (NO_LTO twin builddir)
  python3 tools/check-parity.py (--report | --require-all)  # cutover gate
EOF
    exit 0
fi

ARCH_GUESS="$(basename "$BUILD_DIR" | sed 's/^meson-//; s/-nolto$//; s/-release$//')"
echo "  BUILD  $BUILD_DIR"
if ! python3 "$ROOT/tools/validate_style.py" --quiet; then
    echo "STYLE GATE FAILED (Errors > 0)" >&2
    exit 1
fi
MESON_BIN="$(command -v meson || true)"
if [ -z "$MESON_BIN" ]; then
    echo "ERROR: meson not found (pip install 'meson==1.12.0')" >&2
    exit 1
fi
if [ ! -f "$BUILD_DIR/build.ninja" ]; then
    echo "ERROR: $BUILD_DIR not configured (meson setup first)" >&2
    exit 1
fi
MODE_MARKER="$BUILD_DIR/.lto-mode-$ARCH_GUESS"
if [ -f "$MODE_MARKER" ]; then
    WANT_LTO="on"
    if "$MESON_BIN" introspect "$BUILD_DIR" --buildoptions 2>/dev/null | grep -q '"name": "b_lto"[^}]*"value": false'; then
        WANT_LTO="off"
    fi
    if ! grep -q "^lto=$WANT_LTO " "$MODE_MARKER" 2>/dev/null; then
        echo "[HOST] .lto-mode mismatch — wiping $BUILD_DIR and reconfiguring"
        rm -rf "$BUILD_DIR"
        echo "ERROR: wiped stale builddir; re-run meson setup, then $0" >&2
        exit 2
    fi
fi
exec "$MESON_BIN" compile -C "$BUILD_DIR"
