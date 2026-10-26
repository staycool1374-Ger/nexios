#!/usr/bin/env bash
# check_makefile_freeze.sh — Makefile freeze gate (issue #170 A1/X1).
#
# During the dual period the frozen build (Makefile, mk/) is read-only:
# any change set touching those paths must carry a Meson mirror or a
# parity-ledger update, or the gate fails. After the single delete commit
# removes the frozen build, the guarded paths are gone and this gate
# passes trivially (nothing to freeze).
#
# The guarded paths are named here because guarding them is this script's
# only job; it is exempt from check_no_make_refs.sh for that reason.
# No build is invoked from here — this script only inspects the change set.
#
# Usage (CI): bash tools/check_makefile_freeze.sh \
#   --event <push|pull_request> --ref <ref> --base-ref <base> \
#   --head-ref <head> --before <sha>
# Local simulation: bash tools/check_makefile_freeze.sh --range A..B
set -u

EVENT=""
REF_NAME=""
BASE_REF=""
HEAD_REF=""
BEFORE_SHA=""
RANGE=""

while [ $# -gt 0 ]; do
    case "$1" in
        --event) EVENT="${2:-}"; shift 2 ;;
        --ref) REF_NAME="${2:-}"; shift 2 ;;
        --base-ref) BASE_REF="${2:-}"; shift 2 ;;
        --head-ref) HEAD_REF="${2:-}"; shift 2 ;;
        --before) BEFORE_SHA="${2:-}"; shift 2 ;;
        --range) RANGE="${2:-}"; shift 2 ;;
        --help) sed -n '2,16p' "$0"; exit 0 ;;
        *) echo "check_makefile_freeze: unknown arg: $1" >&2; exit 2 ;;
    esac
done

if [ -n "$RANGE" ]; then
    DIFF_SPEC="$RANGE"
elif [ "$EVENT" = "pull_request" ]; then
    if [ -z "$BASE_REF" ]; then
        echo "check_makefile_freeze: pull_request needs --base-ref" >&2
        exit 2
    fi
    DIFF_SPEC="origin/${BASE_REF}...HEAD"
else
    case "$BEFORE_SHA" in
        ''|0000000000000000000000000000000000000000)
            DIFF_SPEC="origin/main...HEAD" ;;
        *) DIFF_SPEC="${BEFORE_SHA}..HEAD" ;;
    esac
fi

# shellcheck disable=SC2086
CHANGED=$(git diff --name-only $DIFF_SPEC 2>/dev/null || true)
if [ -z "$CHANGED" ]; then
    echo "check_makefile_freeze: clean (no changes in $DIFF_SPEC)"
    exit 0
fi

FROZEN_TOUCHED=$(printf '%s\n' "$CHANGED" | grep -E '^(Makefile|mk/)' || true)
if [ -z "$FROZEN_TOUCHED" ]; then
    echo "check_makefile_freeze: clean (frozen build untouched)"
    exit 0
fi

MIRROR_TOUCHED=$(printf '%s\n' "$CHANGED" | grep -E '^(meson\.build|meson\.options|cross/|src/meson\.build|packaging/meson\.build|tests/meson\.build|tools/meson/|docs/specs/build-system-parity\.tsv)' || true)
if [ -n "$MIRROR_TOUCHED" ]; then
    echo "check_makefile_freeze: frozen build touched WITH Meson mirror/ledger update — allowed"
    printf '%s\n' "$FROZEN_TOUCHED" | sed 's/^/  frozen: /'
    printf '%s\n' "$MIRROR_TOUCHED" | sed 's/^/  mirror: /'
    exit 0
fi

echo "check_makefile_freeze: FROZEN BUILD TOUCHED WITHOUT MIRROR — FAIL" >&2
printf '%s\n' "$FROZEN_TOUCHED" | sed 's/^/  frozen: /' >&2
echo "mirror the change under meson.build, meson.options, cross/," >&2
echo "src/meson.build, packaging/meson.build, tests/meson.build," >&2
echo "tools/meson/, or update docs/specs/build-system-parity.tsv" >&2
exit 1
