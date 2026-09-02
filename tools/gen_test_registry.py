#!/usr/bin/env python3
"""Generate a static test registry header from C++ test files.

Scans a directory tree for .cpp files, extracts JARVIS_TEST() macro
invocations, and produces test_registry.gen.hpp with a static inline
array of TestMeta entries.

Pattern A (with metadata):
    JARVIS_TEST(name, "PRE: conds | POST: conds") { ... }

Pattern B (legacy, no metadata):
    JARVIS_TEST(name) { ... }
"""

import argparse
import os
import re
import sys

RE_TEST = re.compile(
    r'JARVIS_TEST\s*\(\s*'
    r'(\w+)\s*'
    r'(?:,\s*"([^"]*)"\s*)?'
    r'\)',
    re.DOTALL,
)

RE_META = re.compile(
    r'PRE:\s*(.*?)\s*\|\s*POST:\s*(.*)',
    re.DOTALL,
)

DEFAULT_PRE = "none"
DEFAULT_POST = "none"


def parse_metadata(raw: str | None):
    if raw is None:
        return DEFAULT_PRE, DEFAULT_POST
    m = RE_META.search(raw)
    if m:
        return m.group(1).strip(), m.group(2).strip()
    return raw.strip(), DEFAULT_POST


def split_daemons(raw: str):
    if not raw or raw.strip().lower() == "none":
        return []
    return [d.strip() for d in raw.split(",") if d.strip()]


def strip_comments(text: str) -> str:
    """Remove C++ single-line comments (// ...) from the text.
    Multi-line comments (/* ... */) are preserved; JARVIS_TEST is
    never inside them in practice."""
    result: list[str] = []
    in_string = False
    in_char = False
    for line in text.splitlines(keepends=True):
        i = 0
        n = len(line)
        out = []
        while i < n:
            c = line[i]
            if c == '"' and not in_char:
                in_string = not in_string
            elif c == "'" and not in_string:
                in_char = not in_char
            if not in_string and not in_char and i + 1 < n:
                if c == '/' and line[i + 1] == '/':
                    # Skip the rest of the line but preserve newline
                    if line.endswith("\r\n"):
                        out.append("\r\n")
                    elif line.endswith("\n"):
                        out.append("\n")
                    break
            out.append(c)
            i += 1
        result.append("".join(out))
    return "".join(result)


# Known compile-time config macros and their default (non-zero) values.
# When the scanner encounters #if MACRO and MACRO is not in this map, it
# assumes the branch is taken (conservative: includes everything).
# Add entries here when a CONFIG_* macro gates test registration.
_CONFIG_TRUE = frozenset({
    # Add config macros that are 1 by default and gate test registration:
})

def _is_config_disabled(cond: str) -> bool:
    """Return True if `cond` is a known-disabled config conditional."""
    # Literal #if 0
    if cond == "0":
        return True
    # #if CONFIG_XXX where the config is 0 by default
    if cond.startswith("CONFIG_") or cond.startswith("!CONFIG_"):
        negated = cond.startswith("!")
        name = cond.lstrip("!")
        # If it's in _CONFIG_TRUE, it's enabled -> not disabled
        if name in _CONFIG_TRUE:
            return negated  # #if !ENABLED -> disabled, #if ENABLED -> not disabled
        # Not in the known-true set -> assume 0 by default
        return not negated  # #if UNKNOWN -> disabled, #if !UNKNOWN -> not disabled
    # Unknown condition — assume enabled (conservative)
    return False


# ---------------------------------------------------------------------------
# Architecture-aware preprocessing for the test registry scanner.
#
# The scanner must not emit tests whose registration lives inside a
# `#if defined(CONFIG_ARCH_*)` block for a *foreign* architecture: those test
# functions are not compiled on the target arch, so referencing them from the
# generated registry would produce undefined symbols at link time.
#
# Three per-level modes are tracked in a stack:
#   SCAN  — legacy passthrough; every nested branch is scanned (unknown
#           conditions are kept conservatively, matching the original tool).
#   DROP  — entered for legacy-disabled conditions (#if 0, unknown CONFIG_*).
#           The whole level is skipped and nested branches are NOT re-evaluated.
#   ARCH  — the condition is a pure architecture expression
#           (defined(CONFIG_ARCH_X), !defined(...), bare CONFIG_ARCH_X,
#           combined with && / ||).  It is evaluated against the target
#           architecture.  An `&&` chain whose *leftmost* arch term is false
#           for the target is treated as disabled (covers
#           `defined(CONFIG_ARCH_X86_64) && CONFIG_SMAP` without affecting
#           x86_64, where the term is true and falls through to legacy keep).
# ---------------------------------------------------------------------------
_ARCH_TERM_RE = re.compile(
    r'^(!)?\s*'
    r'(?:defined\s*\(\s*(CONFIG_ARCH_[A-Z0-9_]+)\s*\)'
    r'|(CONFIG_ARCH_[A-Z0-9_]+))'
    r'\s*$'
)

_ARCH_OP_RE = re.compile(r'\s*(&&|\|\|)\s*')


def _eval_arch_condition(cond: str, target_arch: str) -> tuple[bool, bool]:
    """Evaluate a preprocessor condition as a pure arch expression.

    Returns (is_arch_expr, result).  is_arch_expr is True only when every
    term in the expression is an arch macro reference (possibly negated),
    combined with && / ||.  For the compound `leftmost-arch-false && rest`
    case (rest is not a pure arch term) it returns (True, False) so the
    foreign-arch block is dropped without changing x86_64, where the leftmost
    term is true and the expression falls through to legacy handling.
    """
    own = "CONFIG_ARCH_" + target_arch.upper()
    parts = _ARCH_OP_RE.split(cond)
    ops = [p for p in parts if p in ("&&", "||")]
    terms = [p.strip() for p in parts if p not in ("&&", "||") and p.strip()]
    if not terms:
        return (False, False)

    # Try to parse every term as a pure arch macro reference.
    parsed: list[tuple[str, bool] | None] = []  # (name, negated) or None
    all_arch = True
    for t in terms:
        m = _ARCH_TERM_RE.match(t)
        if m is None:
            all_arch = False
            parsed.append(None)
        else:
            parsed.append((m.group(2) or m.group(3), bool(m.group(1))))
    if all_arch:
        # Pure arch boolean expression — evaluate it.  All entries are
        # guaranteed non-None here.
        arch_terms: list[tuple[str, bool]] = []
        for entry in parsed:
            assert entry is not None
            arch_terms.append(entry)
        result = (arch_terms[0][0] == own)
        if arch_terms[0][1]:
            result = not result
        for i, op in enumerate(ops):
            tv = (arch_terms[i + 1][0] == own)
            if arch_terms[i + 1][1]:
                tv = not tv
            result = (result and tv) if op == "&&" else (result or tv)
        return (True, result)

    # Not a pure arch expression.  Apply the leftmost-arch-false && rule:
    # an `&&` chain whose leftmost term is a (non-negated) arch macro of a
    # foreign architecture is disabled for the target.  This covers
    # `defined(CONFIG_ARCH_X86_64) && CONFIG_SMAP` (CONFIG_SMAP is 0 on
    # non-x86_64) without changing x86_64, where the leftmost term is true.
    if "||" not in cond:
        m = _ARCH_TERM_RE.match(terms[0])
        if m is not None and not bool(m.group(1)):
            name = m.group(2) or m.group(3)
            if name != own:
                return (True, False)
    return (False, False)


def strip_disabled_blocks(text: str, target_arch: str = "x86_64") -> str:
    """Remove disabled preprocessor blocks (#if 0, #if CONFIG_0, foreign-arch
    #if defined(CONFIG_ARCH_*) blocks, etc.) from C++ source.

    The per-level mode stack preserves the legacy conservative behavior for
    non-arch conditions (SCAN/DROP) while correctly evaluating pure arch
    expressions (ARCH) against `target_arch`.
    """
    result: list[str] = []
    # Stack of levels; each entry is a tuple (mode, active):
    #   ("scan", True)        — legacy passthrough, content kept
    #   ("drop", False)       — disabled, content dropped, never re-scanned
    #   ("arch", bool)        — arch expr; True => this branch is live
    stack: list[tuple] = []
    for line in text.splitlines(keepends=True):
        stripped = line.strip()
        if stripped.startswith("#if "):
            raw_cond = stripped[4:].strip()
            comment_pos = raw_cond.find("//")
            if comment_pos >= 0:
                raw_cond = raw_cond[:comment_pos].strip()
            if any(mode == "drop" for mode, _ in stack):
                stack.append(("drop", False))
                continue
            # Legacy disabled check first (matches original tool).
            if _is_config_disabled(raw_cond):
                stack.append(("drop", False))
                continue
            is_arch, arch_val = _eval_arch_condition(raw_cond, target_arch)
            if is_arch:
                stack.append(("arch", arch_val))
            else:
                stack.append(("scan", True))
            continue
        if stripped.startswith("#ifdef ") or stripped.startswith("#ifndef "):
            if any(mode == "drop" for mode, _ in stack):
                stack.append(("drop", False))
                continue
            # #ifdef CONFIG_ARCH_X is equivalent to the arch expression
            # defined(CONFIG_ARCH_X); #ifndef to !defined(...).  Treat them
            # as arch conditions when the macro is a CONFIG_ARCH_* macro.
            macro = stripped.split(None, 1)[1].strip()
            if macro.startswith("CONFIG_ARCH_"):
                is_defined = stripped.startswith("#ifdef")
                cond = ("defined(" + macro + ")" if is_defined
                        else "!defined(" + macro + ")")
                is_arch, arch_val = _eval_arch_condition(cond, target_arch)
                if is_arch:
                    stack.append(("arch", arch_val))
                    continue
            stack.append(("scan", True))
            continue
        if stripped.startswith("#elif"):
            if not stack:
                continue
            if stack[-1][0] == "drop":
                continue
            if stack[-1][0] == "arch":
                if stack[-1][1]:
                    # A prior branch was taken — this #elif branch is dead.
                    stack[-1] = ("arch", False)
                else:
                    raw_elif = stripped.lstrip("#elif").strip()
                    comment_pos = raw_elif.find("//")
                    if comment_pos >= 0:
                        raw_elif = raw_elif[:comment_pos].strip()
                    is_arch, arch_val = _eval_arch_condition(
                        raw_elif, target_arch)
                    if is_arch:
                        stack[-1] = ("arch", arch_val)
                    else:
                        # Unknown arch expr in #elif — conservative keep.
                        stack[-1] = ("arch", True)
            # SCAN levels keep their pass-through behavior (#elif ignored).
            continue
        if stripped.startswith("#else"):
            if not stack:
                continue
            if stack[-1][0] == "arch":
                stack[-1] = ("arch", not stack[-1][1])
            elif stack[-1][0] == "drop":
                continue
            # SCAN levels keep pass-through.
            continue
        if stripped.startswith("#endif"):
            if stack:
                stack.pop()
            continue
        # Content line: emit unless an enclosing level suppresses it.
        if all((mode == "scan") or (mode == "arch" and active)
               for mode, active in stack):
            result.append(line)
    return "".join(result)


def scan_tests(root_dir: str, file_list: list[str] | None = None,
               target_arch: str = "x86_64"):
    tests = []
    sources: list[str] = []
    if file_list:
        for f in file_list:
            f = f.strip()
            if f and f.endswith(".cpp"):
                sources.append(f)
    else:
        for dirpath, _, filenames in os.walk(root_dir):
            for fn in filenames:
                if fn.endswith(".cpp"):
                    sources.append(os.path.join(dirpath, fn))

    for path in sources:
        if not os.path.exists(path):
            print(f"warning: {path} not found, skipping", file=sys.stderr)
            continue
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                content = f.read()
        except OSError as e:
            print(f"error: cannot read {path}: {e}", file=sys.stderr)
            continue
        content = strip_comments(content)
        content = strip_disabled_blocks(content, target_arch)
        for m in RE_TEST.finditer(content):
            name = m.group(1)
            meta_raw = m.group(2)
            pre, post = parse_metadata(meta_raw)
            tests.append((name, pre, post))
    return tests


def gen_setup(name: str, daemons: list[str]):
    if not daemons:
        return f"inline void setup_{name}() {{}}"
    calls = "\n        ".join(
        f'kernel::daemon::ensure_running("{d}");' for d in daemons
    )
    return f"inline void setup_{name}() {{\n        {calls}\n    }}"


def gen_teardown(name: str, daemons: list[str]):
    if not daemons:
        return f"inline void teardown_{name}() {{}}"
    calls = "\n        ".join(
        f'kernel::daemon::terminate("{d}");' for d in daemons
    )
    return f"inline void teardown_{name}() {{\n        {calls}\n    }}"


def generate(tests, output_path: str):
    lines = ["#pragma once"]

    for name, _, _ in tests:
        lines.append(f"void test_{name}();")
    lines.append("")

    for name, pre_raw, post_raw in tests:
        pre_daemons = split_daemons(pre_raw)
        post_daemons = split_daemons(post_raw)
        lines.append(gen_setup(name, pre_daemons))
        lines.append(gen_teardown(name, post_daemons))
    lines.append("")

    lines.append(
        "struct TestMeta {"
        " const char* name;"
        " const char* pre_conditions;"
        " const char* post_conditions;"
        " void (*test_func)();"
        " void (*setup_func)();"
        " void (*teardown_func)();"
        " };"
    )
    lines.append("")

    lines.append("inline const TestMeta generated_tests[] = {")
    for name, pre, post in tests:
        lines.append(
            f'  {{"{name}", "{pre}", "{post}",'
            f' &test_{name}, &setup_{name}, &teardown_{name}}},'
        )
    lines.append("};")
    lines.append("")

    lines.append(
        "inline constexpr size_t generated_tests_count = "
        "sizeof(generated_tests) / sizeof(TestMeta);"
    )
    lines.append("")

    content = "\n".join(lines) + "\n"
    try:
        with open(output_path, "w", encoding="utf-8") as f:
            f.write(content)
    except OSError as e:
        print(f"error: cannot write {output_path}: {e}", file=sys.stderr)
        sys.exit(1)
    print(f"generated {output_path}: {len(tests)} tests")


def main():
    ap = argparse.ArgumentParser(
        description="Generate a static test registry header from C++ test files."
    )
    ap.add_argument(
        "input_dir",
        help="Root directory to scan for .cpp files",
    )
    ap.add_argument(
        "output_file",
        help="Path for the generated header (e.g. test_registry.gen.hpp)",
    )
    ap.add_argument(
        "--file-list",
        help="Path to a file listing source files to scan (one per line). "
             "When set, only these files are scanned instead of walking input_dir.",
    )
    ap.add_argument(
        "--arch",
        default="x86_64",
        help="Target architecture (x86_64, aarch64, riscv64). Tests inside "
             "foreign-arch #if defined(CONFIG_ARCH_*) blocks are not emitted "
             "into the registry, since they are not compiled on this arch.",
    )
    args = ap.parse_args()

    if not os.path.isdir(args.input_dir):
        print(f"error: input directory '{args.input_dir}' not found", file=sys.stderr)
        sys.exit(1)

    file_list = None
    if args.file_list:
        try:
            with open(args.file_list, "r") as f:
                file_list = [line for line in f if line.strip()]
        except OSError as e:
            print(f"error: cannot read file list '{args.file_list}': {e}",
                  file=sys.stderr)
            sys.exit(1)

    tests = scan_tests(args.input_dir, file_list, args.arch)
    tests.sort(key=lambda t: t[0])
    generate(tests, args.output_file)


if __name__ == "__main__":
    main()
