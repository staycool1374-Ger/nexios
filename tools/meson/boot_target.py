#!/usr/bin/env python3
"""Interactive boot run_target (issue #170 follow-up).

Usage (via run_target only — never in CI):
    python3 tools/meson/boot_target.py --mode debug|release --arch ARCH
        --builddir DIR

Boots the builddir kernel interactively: the default display stays on (a
framebuffer window opens where a display server runs), the serial console
is stdio (mon:stdio), there is no GDB stub and no watchdog verdict — the
guest runs until the user quits QEMU.

Fail-closed contract (every violation exits non-zero before QEMU starts):
- arch must be x86_64 (other arches boot a raw ELF, a different flow).
- --mode must match the builddir buildtype (boot-debug runs only on
  debug builddirs, boot-release only on release ones).
- the baked test class must be `none`: a builddir with a class baked in
  fails loud instead of booting into a suite unasked.
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qemu_test as harness  # noqa: E402  (argv builder + tag)


def fail(msg):
    """Print a single actionable error line and exit non-zero."""
    print("boot_target: ERROR: %s" % msg, file=sys.stderr)
    return 2


def live_lto(builddir):
    """Read the configured b_lto flag via meson introspect."""
    out = subprocess.run(
        ["meson", "introspect", str(builddir), "--buildoptions"],
        capture_output=True, text=True, check=True,
    )
    buildtype, lto = "", True
    for option in json.loads(out.stdout):
        if option.get("name") == "buildtype":
            buildtype = option.get("value")
        if option.get("name") == "b_lto":
            lto = bool(option.get("value"))
    return buildtype, lto


def main():
    """Assemble the interactive QEMU command and exec into it."""
    ap = argparse.ArgumentParser(description="boot a builddir kernel interactively")
    ap.add_argument("--mode", required=True, choices=["debug", "release"])
    ap.add_argument("--arch", required=True)
    ap.add_argument("--builddir", required=True)
    args = ap.parse_args()
    if args.arch != "x86_64":
        return fail("interactive boot is x86_64-only (got %s)" % args.arch)
    builddir = Path(args.builddir).resolve()
    iso = builddir / "src" / "nexios-rtos.iso"
    if not iso.exists():
        return fail("missing built ISO %s (build the builddir first)" % iso)
    buildtype, lto = live_lto(builddir)
    want = "release" if args.mode == "release" else "debug"
    if buildtype != want:
        return fail("boot-%s needs a %s builddir (this one is %s)"
                    % (args.mode, want, buildtype))
    baked = builddir / (".baked-test-config-%s" % args.arch)
    try:
        test_class = baked.read_text(encoding="utf-8").strip()
    except OSError:
        return fail("no baked test-config %s (rebuild the builddir first)"
                    % baked)
    if test_class != "none":
        return fail("builddir bakes test class %r — refusing to boot into "
                    "a suite unasked (reconfigure -Dtest_class=none and "
                    "rebuild for interactive boot)" % test_class)
    if args.mode == "release" and lto:
        # Make parity: release links without LTO (LTO release hits the
        # inline-only memset link failure; first bug to fix if LTO releases
        # are ever wanted). Refuse rather than silently boot a mixed tree.
        return fail("release builddir has -Db_lto=true (release is no-LTO "
                    "by design; reconfigure with -Db_lto=false)")

    tag = harness.checkout_tag()
    paths = {
        "serial": str(builddir / "boot-debugcon.log"),
        "com1": str(builddir / "boot-com1.log"),
        "iso": str(iso),
        "fat32": str(builddir / "packaging" / "fat32.img"),
    }
    base = harness.qemu_argv(args.arch, "debug", "none", paths, tag)
    cmd = [base[0]]
    skip_next = False
    for index, token in enumerate(base[1:], start=1):
        if skip_next:
            skip_next = False
            continue
        if token == "-serial":
            cmd += ["-serial", "mon:stdio"]
            skip_next = True
        elif token == "-monitor":
            skip_next = True
        else:
            cmd.append(token)
    if args.mode == "release":
        cmd += ["-smp", "2"]
    for banned in ("-s", "-S", "-gdb"):
        if banned in cmd:
            return fail("internal error: stub flag %s leaked into the "
                        "interactive command" % banned)

    print("boot-%s: %s" % (args.mode, " ".join(cmd)), flush=True)
    env = dict(os.environ)
    env.pop("HRT_ICOUNT", None)
    env.pop("HRT_TRACE", None)
    os.execvpe(cmd[0], cmd, env)
    return 2


if __name__ == "__main__":
    sys.exit(main())
