#!/usr/bin/env bash
# check_no_cross_edits.sh — isolation gate (Phase 1): no cross-role edits.
#
# Dev sessions may not touch the test trees; QE sessions may not touch
# production sources. The live permission blocks (dev.json / qe.json) cover
# the edit/write/patch tools only — bash heredocs, sed -i and python writes
# bypass them by design. This script is the fail-closed backstop: it fails
# the tree (and CI) when the current change set crosses the role fence.
#
# Scans TRACKED files only (git ls-files: build output, vendored deps and
# IDE state are untracked and out of scope by construction — same contract
# as tools/check_no_make_refs.sh).
#
# Change set: tracked files differing from BASE, where BASE is
# `git merge-base main HEAD` (branch work, committed or not) falling back
# to HEAD (dirty tree directly on main). Untracked-but-unadded files are
# invisible until `git add` tracks them; the gate turns red at commit time.
#
# Role resolution: --role dev|qe wins; otherwise $OPENCODE_CONFIG basename
# (dev.json -> dev, qe.json -> qe). No detectable role is a fail-closed
# usage error (exit 2), never a silent pass.
#
# Fences:
#   dev role: changes under src/kernel/test/** or src/test/** fail.
#   qe role:  changes under src/** EXCEPT src/kernel/test/** and
#             src/test/** fail.
#
# Exit-code contract (mirrors check_no_make_refs.sh):
#   0  clean   — prints `check_no_cross_edits: clean`
#   1  violation — prints `CROSS-ROLE EDITS:` plus offending paths
#   2  usage/environment error (unknown role, not a git repo, self-test fail)
#
# Exempt: this file itself (its header documents the patterns it hunts).
#
# Self-test: --self-test runs the fence classifier over built-in cases in
# both directions (green on clean sets, red on deliberate violations) and
# exits 0/2. CI wires the self-test; humans run the role check pre-commit.
set -u
cd "$(dirname "$0")/.."

ROLE=""
SELFTEST=0
while [ $# -gt 0 ]; do
    case "$1" in
        --role=dev|--role=qe) ROLE="${1#--role=}" ;;
        --role)
            case "${2:-}" in
                dev|qe) ROLE="$2"; shift ;;
                *) echo "usage: $0 [--role dev|qe] [--self-test]" >&2; exit 2 ;;
            esac
            ;;
        --self-test) SELFTEST=1 ;;
        -h|--help) sed -n '1,32p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1 (usage: $0 [--role dev|qe] [--self-test])" >&2; exit 2 ;;
    esac
    shift
done

# classify <role> <path>: prints VIOLATION or OK.
classify() {
    case "$1" in
        dev)
            case "$2" in
                src/kernel/test/*|src/test/*) echo "VIOLATION" ;;
                *) echo "OK" ;;
            esac
            ;;
        qe)
            case "$2" in
                src/kernel/test/*|src/test/*) echo "OK" ;;
                src/*) echo "VIOLATION" ;;
                *) echo "OK" ;;
            esac
            ;;
    esac
}

if [ "$SELFTEST" = "1" ]; then
    fail=0
    # format: role|path|expected
    while IFS='|' read -r role path expected; do
        [ -z "$role" ] && continue
        got=$(classify "$role" "$path")
        if [ "$got" = "$expected" ]; then
            printf 'PASS role=%s path=%s -> %s\n' "$role" "$path" "$got"
        else
            printf 'FAIL role=%s path=%s -> %s (want %s)\n' \
                "$role" "$path" "$got" "$expected"
            fail=1
        fi
    done <<'CASES'
dev|src/kernel/test/probe.tmp|VIOLATION
dev|src/kernel/test/nested/deep/probe.tmp|VIOLATION
dev|src/test/foo.cpp|VIOLATION
dev|src/kernel/sched/probe.tmp|OK
dev|tools/meson/gdb_run.py|OK
dev|test-history.txt|OK
qe|src/kernel/sched/probe.tmp|VIOLATION
qe|src/kernel/sched/nested/probe.tmp|VIOLATION
qe|src/kernel/test/probe.tmp|OK
qe|src/kernel/test/nested/deep/probe.tmp|OK
qe|src/test/foo.cpp|OK
qe|tools/check_no_cross_edits.sh|OK
qe|test-history.txt|OK
CASES
    if [ "$fail" = "0" ]; then
        echo "check_no_cross_edits self-test: clean"
        exit 0
    fi
    echo "check_no_cross_edits self-test: FAILURES" >&2
    exit 2
fi

if [ -z "$ROLE" ]; then
    case "${OPENCODE_CONFIG:-}" in
        *dev.json) ROLE="dev" ;;
        *qe.json) ROLE="qe" ;;
        *)
            echo "check_no_cross_edits: role undetectable" >&2
            echo "(set OPENCODE_CONFIG to dev.json/qe.json or pass --role dev|qe)" >&2
            exit 2
            ;;
    esac
fi

BASE=$(git merge-base main HEAD 2>/dev/null || git rev-parse HEAD 2>/dev/null || true)
if [ -z "$BASE" ]; then
    echo "check_no_cross_edits: not a git repo" >&2
    exit 2
fi

TMP_CHANGED=$(mktemp)
TMP_TRACKED=$(mktemp)
trap 'rm -f "$TMP_CHANGED" "$TMP_TRACKED"' EXIT

# Tracked files changed vs BASE (committed-on-branch + staged + unstaged).
git diff --name-only "$BASE" -- . > "$TMP_CHANGED"
# Intersect with the tracked universe (drops untracked noise by contract).
git ls-files > "$TMP_TRACKED"

OUT=""
while IFS= read -r file; do
    [ -z "$file" ] && continue
    case "$file" in
        tools/check_no_cross_edits.sh) continue ;;  # exempt: this gate itself
    esac
    if ! grep -qxF "$file" "$TMP_TRACKED"; then
        continue
    fi
    if [ "$(classify "$ROLE" "$file")" = "VIOLATION" ]; then
        OUT="${OUT}${ROLE}: ${file}
"
    fi
done < "$TMP_CHANGED"

if [ -n "$OUT" ]; then
    printf 'CROSS-ROLE EDITS:\n%s' "$OUT"
    exit 1
fi
echo "check_no_cross_edits: clean"
