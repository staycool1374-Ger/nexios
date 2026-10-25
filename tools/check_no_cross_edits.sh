#!/usr/bin/env bash
# check_no_cross_edits.sh — isolation gate (Phase 1): no cross-role edits.
#
# Dev sessions may not touch the test trees; QE sessions may not touch
# production sources. The live permission blocks (dev.json / qe.json) cover
# the edit/write/patch tools only — bash heredocs, sed -i and python writes
# bypass them by design. This script is the fail-closed backstop: it fails
# the tree (and CI) when the current change set crosses the role fence.
#
# Change set: tracked files differing from BASE PLUS every untracked,
# non-ignored file (`git ls-files --others --exclude-standard`). Untracked
# files were silently invisible before #412 S4-8: a brand-new
# `src/kernel/test/*.cpp` could be written by a dev session and never seen by
# the gate until `git add`, which is exactly the window the fence exists for.
# BASE is `git merge-base main HEAD` (branch work, committed or not) falling
# back to HEAD (dirty tree directly on main), or an explicit `--base <rev>`;
# an explicit but empty/unresolvable base is a usage error (exit 2), never a
# silent merge-base fallback (S4-7).
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
# This file is NOT exempt: an edit to the gate is classified like any other
# path (it is neither a test tree nor a `src/` source, so it passes for both
# roles) — the old blanket self-exemption hid gate edits from the gate.
#
# Self-test: --self-test runs the fence classifier over built-in cases in
# both directions (green on clean sets, red on deliberate violations) and
# exits 0/2.
# CI: the meson workflow runs `--self-test` (classifier sanity) and the ROLE
# check itself, with the role derived from the branch (`testbed` -> qe, every
# other branch -> dev) and an EXPLICIT --base (a PR checkout has no local
# `main`, so the merge-base fallback would classify an empty set as clean).
# Run it by hand with `--role=dev|qe` before pushing.
set -u
cd "$(dirname "$0")/.." || exit 2

ROLE=""
SELFTEST=0
BASE_OVERRIDE=""
BASE_SET=0
while [ $# -gt 0 ]; do
    case "$1" in
        --role=dev|--role=qe) ROLE="${1#--role=}" ;;
        --role)
            case "${2:-}" in
                dev|qe) ROLE="$2"; shift ;;
                *) echo "usage: $0 [--role dev|qe] [--base <rev>] [--self-test]" >&2; exit 2 ;;
            esac
            ;;
        --base=*)
            BASE_OVERRIDE="${1#--base=}"
            [ -n "$BASE_OVERRIDE" ] || {
                echo "check_no_cross_edits: --base needs a revision (got an empty value)" >&2
                exit 2
            }
            BASE_SET=1 ;;
        --base)
            [ -n "${2:-}" ] || { echo "usage: $0 --base <rev>" >&2; exit 2; }
            BASE_OVERRIDE="$2"; BASE_SET=1; shift ;;
        --self-test) SELFTEST=1 ;;
        -h|--help) sed -n '1,45p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1 (usage: $0 [--role dev|qe] [--base <rev>] [--self-test])" >&2; exit 2 ;;
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
    # --- integration: the change-set builder, in a hermetic scratch repo -----
    # Proves the parts the classifier cases cannot: untracked files are in scope
    # (S4-8), renames classify on the NEW path, and an explicit empty base is a
    # usage error rather than a merge-base fallback (S4-7).
    scratch=$(mktemp -d)
    trap 'rm -rf "$scratch"' EXIT
    mkdir -p "$scratch/tools" "$scratch/src/kernel/sched"
    cp "$0" "$scratch/tools/check_no_cross_edits.sh"
    git -C "$scratch" init -q -b main
    printf 'base\n' > "$scratch/src/kernel/sched/a.cpp"
    git -C "$scratch" add -A
    git -C "$scratch" -c user.email=t@t -c user.name=t commit -qm base
    mkdir -p "$scratch/src/kernel/test"
    printf 'x\n' > "$scratch/src/kernel/test/untracked.cpp"
    if bash "$scratch/tools/check_no_cross_edits.sh" --role=dev --base=main \
        >/dev/null 2>&1; then
        echo "FAIL integration: untracked dev test file not classified" >&2
        fail=1
    else
        printf 'PASS integration: untracked dev test file caught\n'
    fi
    rm -f "$scratch/src/kernel/test/untracked.cpp"
    printf 'y\n' > "$scratch/src/kernel/sched/untracked.cpp"
    if bash "$scratch/tools/check_no_cross_edits.sh" --role=qe --base=main \
        >/dev/null 2>&1; then
        echo "FAIL integration: untracked qe source file not classified" >&2
        fail=1
    else
        printf 'PASS integration: untracked qe source file caught\n'
    fi
    rm -f "$scratch/src/kernel/sched/untracked.cpp"
    git -C "$scratch" mv src/kernel/sched/a.cpp \
        "src/kernel/test/renamed with space.cpp"
    if bash "$scratch/tools/check_no_cross_edits.sh" --role=dev --base=main \
        >/dev/null 2>&1; then
        echo "FAIL integration: rename into the test tree not classified" >&2
        fail=1
    else
        printf 'PASS integration: rename into the test tree caught\n'
    fi
    git -C "$scratch" mv "src/kernel/test/renamed with space.cpp" \
        src/kernel/sched/a.cpp
    bash "$scratch/tools/check_no_cross_edits.sh" --role=dev --base= \
        >/dev/null 2>&1
    if [ $? -eq 2 ]; then
        printf 'PASS integration: empty --base is a usage error\n'
    else
        echo "FAIL integration: empty --base did not exit 2" >&2
        fail=1
    fi
    if bash "$scratch/tools/check_no_cross_edits.sh" --role=dev --base=main \
        >/dev/null 2>&1; then
        printf 'PASS integration: clean tree passes\n'
    else
        echo "FAIL integration: clean tree reported a violation" >&2
        fail=1
    fi

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

if [ "$BASE_SET" = 1 ]; then
    # CI passes the true change-set base; an unresolvable base is a hard error
    # so a single-ref checkout can never classify an empty set as clean.
    BASE=$(git rev-parse --verify --quiet "$BASE_OVERRIDE^{commit}") || {
        echo "check_no_cross_edits: base '$BASE_OVERRIDE' does not resolve" >&2
        exit 2
    }
else
    BASE=$(git merge-base main HEAD 2>/dev/null || git rev-parse HEAD 2>/dev/null || true)
fi
if [ -z "$BASE" ]; then
    echo "check_no_cross_edits: not a git repo" >&2
    exit 2
fi

TMP_CHANGED=$(mktemp)
trap 'rm -f "$TMP_CHANGED"' EXIT

# Tracked files changed vs BASE (committed-on-branch + staged + unstaged) plus
# untracked non-ignored files (build output stays out via --exclude-standard).
git diff --name-only "$BASE" -- . > "$TMP_CHANGED" || {
    echo "check_no_cross_edits: git diff failed (cannot establish the change set)" >&2
    exit 2
}
git ls-files --others --exclude-standard >> "$TMP_CHANGED" || {
    echo "check_no_cross_edits: git ls-files failed (cannot establish the change set)" >&2
    exit 2
}
sort -u -o "$TMP_CHANGED" "$TMP_CHANGED"

OUT=""
while IFS= read -r file; do
    [ -z "$file" ] && continue
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
