#!/usr/bin/env python3
"""Registry codegen parity vs the Makefile flow (issue #170).

Usage:
    python3 tools/meson/tests/test_gen_registry.py <builddir> <arch>

Checks:
  1. The Meson-configured file list (builddir .test-file-list-<arch>)
     is byte-identical to the Make-order list (same `find` the Makefile
     uses for this subtree — identical readdir order by construction).
  2. The Meson-built test_registry.gen.hpp is byte-identical to a
     reference header produced by running tools/gen_test_registry.py over
     that list with the same --arch filter.

Any delta means the registry would register a different test set under
Meson — fail-closed, no boot with a divergent registry.
"""

import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent.parent
GEN = REPO / "tools" / "gen_test_registry.py"
TEST_SRC_DIR = REPO / "src" / "kernel" / "test"


FPU_EXCLUDED = {
    "test_fpu.cpp",
    "test_fpu_clone.cpp",
    "test_fpu_multi.cpp",
    "test_fpu_sse.cpp",
    "test_fpu_xmm_all.cpp",
}


def make_order_list():
    """Replicate the Makefile enumeration for this subtree.

    TEST_SRC_FILES filters SRC_CXX (which already excludes the 5 FPU/SSE
    TUs per mk/rules.mk:26-29 — they fail to compile with GCC 16), so the
    enumeration drops them too. Scanning them would register tests with
    no definitions (non-LTO link failure).
    """
    out = subprocess.run(
        ["find", str(TEST_SRC_DIR), "-maxdepth", "1", "-name", "*.cpp"],
        capture_output=True,
        text=True,
        check=True,
    )
    return [line for line in out.stdout.splitlines()
            if line and Path(line).name not in FPU_EXCLUDED]


def main():
    """Byte-compare the file list and the generated registry header."""
    if len(sys.argv) != 3:
        print("usage: test_gen_registry.py <builddir> <arch>")
        return 2
    builddir = Path(sys.argv[1])
    arch = sys.argv[2]
    candidates = sorted(builddir.rglob(".test-file-list-%s" % arch))
    if not candidates:
        print("no configured file list in %s" % builddir)
        return 2
    meson_list_path = candidates[0]
    meson_list = meson_list_path.read_text(encoding="utf-8").splitlines()
    lines = make_order_list()
    # Normalize to repo-relative form for comparison (both absolute here).
    make_list = [str(Path(p).resolve()) for p in lines]
    meson_abs = [str((REPO / p).resolve()) if not Path(p).is_absolute() else p
                 for p in meson_list if p]
    if meson_abs != make_list:
        print("file-list drift: meson=%d make=%d" % (len(meson_abs), len(make_list)))
        only_meson = sorted(set(meson_abs) - set(make_list))
        only_make = sorted(set(make_list) - set(meson_abs))
        if only_meson:
            print("  meson-only: %s" % only_meson[:5])
        if only_make:
            print("  make-only: %s" % only_make[:5])
        order = [p for p in meson_abs if p in make_list]
        if order != make_list:
            print("  order differs")
        return 1
    headers = sorted(builddir.rglob("test_registry.gen.hpp"))
    if not headers:
        print("meson header not built under %s (run meson compile first)"
              % builddir)
        return 2
    meson_header = headers[0]
    with tempfile.TemporaryDirectory() as tmp:
        ref_list = Path(tmp) / "file-list.txt"
        ref_list.write_text("\n".join(make_list) + "\n", encoding="utf-8")
        ref_header = Path(tmp) / "test_registry.gen.hpp"
        subprocess.run(
            [sys.executable, str(GEN), str(TEST_SRC_DIR), str(ref_header),
             "--file-list", str(ref_list), "--arch", arch],
            check=True,
        )
        built = meson_header.read_bytes()
        expected = ref_header.read_bytes()
    if built != expected:
        print("registry header drift: built=%d bytes reference=%d bytes"
              % (len(built), len(expected)))
        return 1
    print("registry parity PASSED: %d files, %d bytes identical (%s)"
          % (len(meson_abs), len(built), arch))
    return 0


if __name__ == "__main__":
    sys.exit(main())
