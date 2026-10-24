#!/usr/bin/env python3
"""Link-stage artifact contract check (issue #170).

Usage:
    python3 tools/meson/tests/test_stage_paths.py <builddir> <arch>

Asserts every path the link+package stages promise exists and is
non-trivial: patched ELF, .lto-mode marker, baked test-config,
kernel.map, ISO (x86_64), and the legacy staging copies.
"""

import sys
from pathlib import Path


def main():
    """Check link-stage artifacts for one arch builddir."""
    if len(sys.argv) != 3:
        print("usage: test_stage_paths.py <builddir> <arch>")
        return 2
    builddir = Path(sys.argv[1])
    arch = sys.argv[2]
    repo = Path(__file__).resolve().parent.parent.parent.parent
    problems = []

    def need(path, min_size=1024, label=None):
        """Require a non-trivial file, recording problems."""
        name = label or str(path)
        if not path.exists():
            problems.append("missing: %s" % name)
        elif path.stat().st_size < min_size:
            problems.append("too small (%d): %s" % (path.stat().st_size, name))

    need(builddir / "src" / "kernel-debug.elf", label="builddir kernel ELF")
    need(builddir / "kernel.map", label="builddir kernel.map")
    if arch == "x86_64":
        need(builddir / "src" / "nexios-rtos.iso", label="builddir boot ISO")
        iso = builddir / "src" / "nexios-rtos.iso"
        legacy_ok = False
        for name in ("debug", "release"):
            legacy_iso = repo / name / "nexios-rtos.iso"
            if legacy_iso.exists() and iso.exists():
                if legacy_iso.read_bytes() == iso.read_bytes():
                    legacy_ok = True
        if not legacy_ok:
            problems.append("missing or stale: legacy boot ISO "
                            "(debug/ or release/ must byte-match builddir)")
    marker = builddir / (".lto-mode-" + arch)
    if not marker.exists():
        problems.append("missing: .lto-mode marker")
    baked = builddir / (".baked-test-config-" + arch)
    if not baked.exists():
        problems.append("missing: builddir baked test-config")
    need(repo / "build" / "kernel-debug.elf", label="legacy kernel-debug.elf")
    need(repo / "build" / "kernel.elf", label="legacy kernel.elf")
    need(repo / "build" / "kernel.map", label="legacy kernel.map")
    if arch == "riscv64":
        need(builddir / "src" / "kernel-riscv64.bin",
             label="builddir riscv BIN")
        need(repo / "build" / "kernel-riscv64.bin",
             label="legacy riscv BIN")
    if problems:
        print("stage-path FAILED (%s):" % arch)
        for problem in problems:
            print("  - %s" % problem)
        return 1
    print("stage-path PASSED (%s)" % arch)
    return 0


if __name__ == "__main__":
    sys.exit(main())
