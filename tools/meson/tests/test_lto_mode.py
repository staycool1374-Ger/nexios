#!/usr/bin/env python3
"""LTO/buildtype mode marker check (issue #170, A2).

Usage:
    python3 tools/meson/tests/test_lto_mode.py <builddir> <arch>
        [--expect-lto on|off] [--expect-buildtype BUILD]

Asserts builddir/.lto-mode-<arch> matches the requested configuration.
With --expect-lto/--expect-buildtype the marker is compared against the
expectation (used after a reconfigure flip); otherwise it is compared
against the live meson introspection values. The wipe-on-mismatch
behavior itself lives in build.sh/qemu_test.py (Phase 4/6); this test
proves the marker is truthful, which the wipe logic depends on.
"""

import subprocess
import sys
from pathlib import Path


def introspect(builddir):
    """Read live build options from Meson introspection."""
    out = subprocess.run(
        ["meson", "introspect", str(builddir), "--buildoptions"],
        capture_output=True, text=True, check=True,
    )
    import json
    values = {}
    for option in json.loads(out.stdout):
        values[option["name"]] = option["value"]
    return values


def main():
    """Compare the .lto-mode marker against requested or live config."""
    args = sys.argv[1:]
    if len(args) < 2:
        print("usage: test_lto_mode.py <builddir> <arch> "
              "[--expect-lto on|off] [--expect-buildtype BUILD]")
        return 2
    builddir = Path(args[0])
    arch = args[1]
    expect_lto = None
    expect_buildtype = None
    rest = args[2:]
    while rest:
        flag = rest.pop(0)
        if flag == "--expect-lto" and rest:
            expect_lto = rest.pop(0)
        elif flag == "--expect-buildtype" and rest:
            expect_buildtype = rest.pop(0)
        else:
            print("unknown flag: %s" % flag)
            return 2
    marker = builddir / (".lto-mode-" + arch)
    if not marker.exists():
        print("missing marker: %s (build first)" % marker)
        return 2
    content = marker.read_text(encoding="utf-8").strip()
    if expect_lto is None or expect_buildtype is None:
        live = introspect(builddir)
        live_lto = "on" if live.get("b_lto") else "off"
        live_buildtype = live.get("buildtype", "plain")
        if expect_lto is None:
            expect_lto = live_lto
        if expect_buildtype is None:
            expect_buildtype = live_buildtype
    want = "lto=%s buildtype=%s" % (expect_lto, expect_buildtype)
    if content != want:
        print("lto-mode MISMATCH: marker %r != config %r" % (content, want))
        return 1
    print("lto-mode PASSED: %s (%s)" % (marker.name, content))
    return 0


if __name__ == "__main__":
    sys.exit(main())
