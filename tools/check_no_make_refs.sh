#!/usr/bin/env bash
# check_no_make_refs.sh — cutover gate (issue #170 X2): no non-historical
# `make` references outside the frozen build itself.
#
# Scans TRACKED files only (git ls-files: build output, vendored deps and
# IDE state are untracked and out of scope by construction).
#
# Flags (case-sensitive):
#   P1  backticked `make <target>` — doc/command spellings in prose
#       (single-quoted patterns: a literal backtick is used, never `\`` —
#       GNU grep reads `\`` as start-of-buffer anchor, BSD as literal; the
#       bracket-free literal is portable to both).
#   P2  command-position `make <target>` in code files (shell/python/json/
#       awk/CI/ini/exp) — live invocations, restricted to the TARGETS
#       allowlist below so English verbs and tool installs never trip it.
#   P3  bare word `Makefile` — prose pointers at the old build file.
#   P4  backticked `mk/<file>.mk` anchors — pointers into the frozen rules.
#
# Make target allowlist (from the frozen Makefile + documented legacy
# targets): P2 only fires on real targets, so English verbs ("make the",
# "make sure") and apt installs ("git make python3") never trip it.
TARGETS='all|audit|build|check-[a-z-]+|clean|config-summary|coverage[a-z-]*|debug[a-z-]*|distclean|doxygen[a-z-]*|execute-test|gdb|help|install|objdump|picolibc[a-z-]*|profil[a-z-]*|release[a-z-]*|renode[a-z-]*|rr-[a-z]+|run-[a-z-]+|rust[a-z-]*|stack-usage-report|symbols|test[a-z-]*|user-rust-hello'

# Exempt (documented, pruned at cutover — see docs/specs/build-system.md):
#   Makefile, mk/               the frozen build itself (cutover archives it)
#   tools/meson/                parity-implementation provenance comments
#                               ("Mirrors `make ...`", "Source: Makefile ...":
#                               they name the frozen baseline they mirror)
#   docs/specs/build-system.md + build-system-parity.tsv
#                               migration source spec + ledger: describing
#                               Make behavior IS their purpose
#   audits/                     immutable audit trail
#   graphify-out/               generated knowledge-graph cache (versioned
#                               snapshots are history by design; refreshes
#                               via `graphify update .`, not by hand)
#   prompts/ROADMAP_done.md, prompts/README_done.md,
#   prompts/LEARNINGS.md, prompts/LESSONS.md, prompts/BUGS.md,
#   testcases-v0.4.0.md, docs/_archive/
#                               historical archives (never rewritten)
#   AGENTS.md at/after `## Active Summary`, all of prompts/STATE.md
#                               rolling session logs (normative rules live
#                               above the marker / in AGENTS.md proper)
#   .github/workflows/ci.yml + docker-release.yml (dual-period CI; the
#                               cutover flips/deletes them — until then the
#                               Make legs are live, not stale)
#   `#<n> (<title with Makefile>)` issue-title references (history)
#   this file itself (its header documents the patterns it hunts)
#
# English verbs ("make the ...", "make progress") are NOT matched: P2
# requires a target-shaped token and P1 requires backticks. If a future
# comment trips P2, reword the comment — the gate stays strict.
set -u
cd "$(dirname "$0")/.."

TMP_ALLOW=$(mktemp)
TMP_SCAN=$(mktemp)
trap 'rm -f "$TMP_ALLOW" "$TMP_SCAN"' EXIT

# Tracked text files in scope.
git ls-files '*.md' '*.py' '*.sh' '*.json' '*.awk' '*.tsv' '*.ini' \
    '*.yml' '*.yaml' '*.exp' '*.mk' 'Makefile' 'Dockerfile' > "$TMP_ALLOW"

# Exempt paths.
grep -v -e '^Makefile$' -e '^mk/' -e '^tools/meson/' -e '^audits/' \
    -e '^graphify-out/' \
    -e '^docs/_archive/' -e '^docs/doxygen/' \
    -e '^docs/specs/build-system\.md$' \
    -e '^docs/specs/build-system-parity\.tsv$' \
    -e '^prompts/ROADMAP_done\.md$' -e '^prompts/README_done\.md$' \
    -e '^prompts/LEARNINGS\.md$' -e '^prompts/LESSONS\.md$' \
    -e '^prompts/BUGS\.md$' -e '^testcases-v0\.4\.0\.md$' \
    -e '^prompts/STATE\.md$' -e '^tools/check_no_make_refs\.sh$' \
    -e '^\.github/workflows/ci\.yml$' \
    -e '^\.github/workflows/docker-release\.yml$' \
    "$TMP_ALLOW" > "$TMP_SCAN"

fail=0

md_files=$(grep '\.md$' "$TMP_SCAN" || true)
code_files=$(grep -E '\.(py|sh|json|awk|yml|yaml|ini|exp|mk)$|(^|/)Makefile$|(^|/)Dockerfile$' "$TMP_SCAN" || true)

HITS=""
if [ -n "$md_files" ]; then
    # shellcheck disable=SC2086
    # Trailing $'\n' keeps batch outputs separated ($() strips newlines).
    HITS="$HITS$(grep -n -E '[`]make [A-Za-z]' $md_files 2>/dev/null || true)"$'\n'
    HITS="$HITS$(grep -n -E 'Makefile' $md_files 2>/dev/null || true)"$'\n'
    HITS="$HITS$(grep -n -E '[`]mk/[a-z_]+\.mk' $md_files 2>/dev/null || true)"$'\n'
fi
if [ -n "$code_files" ]; then
    # shellcheck disable=SC2086
    HITS="$HITS$(grep -n -E '[`]make [A-Za-z]' $code_files 2>/dev/null || true)"$'\n'
    HITS="$HITS$(grep -n -E "(^|[^a-zA-Z0-9_])make ($TARGETS)([^a-z0-9_.-]|\$)" $code_files 2>/dev/null || true)"$'\n'
    HITS="$HITS$(grep -n -E 'Makefile' $code_files 2>/dev/null || true)"$'\n'
    HITS="$HITS$(grep -n -E '[`]mk/[a-z_]+\.mk' $code_files 2>/dev/null || true)"$'\n'
fi

OUT=""
if [ -n "$HITS" ]; then
    OUT=$(printf '%s\n' "$HITS" | while IFS= read -r line; do
        [ -z "$line" ] && continue
        file=${line%%:*}
        rest=${line#*:}
        if [ "$file" = "AGENTS.md" ]; then
            marker=$(grep -n '^## Active Summary' AGENTS.md | cut -d: -f1)
            lineno=${rest%%:*}
            if [ "$lineno" -ge "$marker" ]; then continue; fi
        fi
        if printf '%s' "$rest" | grep -qE '#[0-9]+ \([^)]*[Mm]akefile[^)]*\)'; then
            continue
        fi
        printf '%s:%s\n' "$file" "$rest"
    done)
fi

if [ -n "$OUT" ]; then
    printf 'STALE MAKE REFERENCES:\n%s\n' "$OUT"
    exit 1
fi
echo "check_no_make_refs: clean"
