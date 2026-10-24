#!/usr/bin/env python3
"""Source-set parity vs the Make discovery (issue #170).

Usage:
    python3 tools/meson/tests/test_source_set.py <builddir> <arch>

Compares the Meson kernel TU set (from introspect --targets) against the
Make-derived set (same find + same exclusions). Order-insensitive set
equality plus count equality; per-TU flags are test_flag_parity's job.
"""

import json
import os
import subprocess
import sys
from pathlib import Path

FPU_EXCLUDED = {
    "src/kernel/test/test_fpu.cpp",
    "src/kernel/test/test_fpu_clone.cpp",
    "src/kernel/test/test_fpu_multi.cpp",
    "src/kernel/test/test_fpu_sse.cpp",
    "src/kernel/test/test_fpu_xmm_all.cpp",
}


def make_set(repo, arch):
    """Replicate mk/rules.mk SRC_CXX for one arch (no external dir)."""
    out = subprocess.run(
        ["find", str(repo / "src"), "-path", "*/kernel/arch", "-prune",
         "-o", "-name", "*.cpp", "-print"],
        capture_output=True, text=True, check=True,
    )
    generic = {p for p in out.stdout.splitlines() if p} - {
        str(repo / "src/kernel/test/test_fpu.cpp"),
        str(repo / "src/kernel/test/test_fpu_clone.cpp"),
        str(repo / "src/kernel/test/test_fpu_multi.cpp"),
        str(repo / "src/kernel/test/test_fpu_sse.cpp"),
        str(repo / "src/kernel/test/test_fpu_xmm_all.cpp"),
    } - FPU_EXCLUDED
    base = subprocess.run(
        ["find", str(repo / "src" / "kernel" / "arch"),
         "-maxdepth", "1", "-name", "*.cpp"],
        capture_output=True, text=True, check=True,
    )
    sub = subprocess.run(
        ["find", str(repo / "src" / "kernel" / "arch" / arch),
         "-name", "*.cpp"],
        capture_output=True, text=True, check=True,
    )
    arch_files = {p for p in (base.stdout + sub.stdout).splitlines() if p}
    return generic | arch_files


def meson_set(builddir, repo):
    """Kernel TU set from Meson introspection (link target only)."""
    out = subprocess.run(
        ["meson", "introspect", str(builddir), "--targets"],
        capture_output=True, text=True, check=True,
    )
    found = set()
    for target in json.loads(out.stdout):
        name = target.get("name", "")
        if name not in ("kernel-unpatched", "phase1_probe"):
            continue
        for group in target.get("target_sources", []):
            for src in group.get("sources", []) + group.get("generated_sources", []):
                path = src if isinstance(src, str) else src.get("path", "")
                if not path:
                    continue
                if not os.path.isabs(path):
                    path = os.path.join(str(repo), path)
                if path.endswith((".cpp", ".c", ".S", ".s", ".asm", ".nasm")):
                    found.add(os.path.normpath(path))
    return found


def main():
    """Compare the Meson and Make TU sets."""
    if len(sys.argv) != 3:
        print("usage: test_source_set.py <builddir> <arch>")
        return 2
    builddir = Path(sys.argv[1])
    arch = sys.argv[2]
    repo = Path(__file__).resolve().parent.parent.parent.parent
    expected = make_set(repo, arch)
    actual = meson_set(builddir, repo)
    actual_abs = set()
    for path in actual:
        candidate = Path(path)
        if candidate.is_absolute():
            actual_abs.add(os.path.normpath(str(candidate)))
        else:
            actual_abs.add(os.path.normpath(os.path.join(str(repo), path)))
    expected_abs = set(os.path.normpath(p) for p in expected)
    missing = sorted(expected_abs - actual_abs)
    extra = sorted(actual_abs - expected_abs)
    # Generated/derived inputs are not Make TUs.
    extra = [p for p in extra
             if "test_registry.gen.hpp" not in p and not p.endswith(".o")]
    if missing or extra:
        print("source-set drift (%s): %d make vs %d meson"
              % (arch, len(expected_abs), len(actual_abs)))
        if missing:
            print("  missing: %s" % missing[:10])
        if extra:
            print("  extra: %s" % extra[:10])
        return 1
    print("source-set parity PASSED: %d TUs (%s)" % (len(expected), arch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
