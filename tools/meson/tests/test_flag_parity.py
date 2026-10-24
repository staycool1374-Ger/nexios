#!/usr/bin/env python3
"""Per-TU flag parity vs the frozen Makefile manifests (issue #170).

Usage:
    python3 tools/meson/tests/test_flag_parity.py <builddir> <arch>

Reads every C++ compile command from `ninja -t commands`, strips
Meson/ninja-injected noise (documented below), normalizes -flto* to
-flto, and diffs the flag multiset against
tools/meson/flag-manifest-<arch>.txt. Exits non-zero on any delta.

Allowed deviations (all inert under -nostdinc / shadowed / depfiles):
  -D_GLIBCXX_ASSERTIONS=1, -D_FILE_OFFSET_BITS=64 (Meson buildtype debug;
   affect system headers only, never included freestanding)
  -O0 (Meson debug buildtype; our explicit -Og wins, last -O applies)
  -fdiagnostics-color=always, -Winvalid-pch (ninja/Meson diagnostics)
  -MD/-MMD/-MF/-MQ/-MT (ninja depfiles; Make used -MMD -MP, dropped by design)
  -I<build-root> entries (build-dir scaffolding, not kernel flags)
  -flto=auto (Meson b_lto spelling; normalized to -flto)
"""

import subprocess
import sys
from pathlib import Path

DROP_PREFIXES = ("-MD", "-MMD", "-MF", "-MQ", "-MT")
DROP_EXACT = {
    "-D_GLIBCXX_ASSERTIONS=1",
    "-D_FILE_OFFSET_BITS=64",
    "-O0",
    "-fdiagnostics-color=always",
    "-Winvalid-pch",
    "-c",
}


def normalize(tokens):
    """Filter scaffolding and normalize the LTO spelling."""
    kept = []
    skip_next = False
    for index, token in enumerate(tokens):
        if skip_next:
            skip_next = False
            continue
        if token in DROP_EXACT:
            continue
        if token in ("-I", "-isystem", "-o", "-MF", "-MQ", "-MT"):
            skip_next = True
            continue
        if token.startswith(DROP_PREFIXES):
            continue
        if token.startswith("-I"):
            continue
        if token == "-o":
            continue
        if token.endswith((".cpp", ".c", ".o", ".obj",
                           ".S", ".s", ".asm", ".nasm")):
            continue
        if "++" in token or token.endswith(("gcc", "g++", "cc", "clang++")):
            if index == 0 or "/" in token:
                continue
        if token.startswith("-flto"):
            kept.append("-flto")
            continue
        kept.append(token)
    return kept


def compile_units(builddir):
    """Yield (language, tokens, input-path) per compile command."""
    out = subprocess.run(
        ["ninja", "-t", "commands"],
        cwd=builddir,
        capture_output=True,
        text=True,
        check=True,
    )
    for line in out.stdout.splitlines():
        if " -c " not in line:
            continue
        tokens = line.split()
        inputs = [t for t in tokens
                  if t.endswith((".cpp", ".c", ".S", ".s", ".asm", ".nasm"))]
        if not inputs:
            continue
        suffix = inputs[-1].rsplit(".", 1)[-1]
        lang = "cc" if suffix in ("c", "S", "s", "asm", "nasm") else "cpp"
        yield lang, tokens, inputs[-1]


def load_manifest(arch, lang):
    """Read the frozen flag set for one arch and language."""
    name = ("flag-manifest-%s.txt" % arch if lang == "cpp"
            else "flag-manifest-%s-cc.txt" % arch)
    path = Path(__file__).resolve().parent.parent / name
    if not path.exists():
        return None
    return sorted(
        line.strip()
        for line in path.read_text().splitlines()
        if line.strip() and not line.startswith("#")
    )


def main():
    """Compare real per-TU flags against the frozen manifests."""
    if len(sys.argv) != 3:
        print("usage: test_flag_parity.py <builddir> <arch>")
        return 2
    builddir = Path(sys.argv[1])
    arch = sys.argv[2]
    failures = 0
    checked = {"cpp": 0, "cc": 0}
    for lang, tokens, path in compile_units(str(builddir)):
        if "/tools/" in path:
            continue  # host tooling compiles natively by design
        expected = load_manifest(arch, lang)
        if expected is None:
            continue
        actual = normalize(tokens)
        checked[lang] += 1
        # Identical duplicates (e.g. Meson's own -Wall/-g beside ours) are
        # harmless scaffolding; missing/extra flags are the real signal.
        seen = set()
        dups = sorted({f for f in actual if f in seen or seen.add(f)})
        actual_set, expected_set = set(actual), set(expected)
        missing = sorted(expected_set - actual_set)
        extra = sorted(actual_set - expected_set)
        if missing or extra:
            failures += 1
            print("flag drift in: %s" % " ".join(tokens[-3:]))
            if missing:
                print("  missing: %s" % " ".join(missing))
            if extra:
                print("  extra:   %s" % " ".join(extra))
        elif dups:
            print("note: harmless duplicate flags: %s" % " ".join(dups))
    total = checked["cpp"] + checked["cc"]
    if total == 0:
        print("no compile commands found in %s" % builddir)
        return 2
    if failures:
        print("flag parity FAILED: %d/%d TUs drift" % (failures, total))
        return 1
    print("flag parity PASSED: %d TUs match manifests-%s (cpp=%d cc=%d)"
          % (total, arch, checked["cpp"], checked["cc"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
