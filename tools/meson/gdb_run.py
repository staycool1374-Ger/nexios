#!/usr/bin/env python3
"""GDB/record-replay flows for the Meson build (issue #170 Phase 4).

Mirrors `make debug-test` / `debug-shell` / `rr-record` / `rr-replay`:
NO_LTO builds (dedicated build/meson-<arch>-nolto builddirs — the A2
escape hatch in action, so GDB flows never churn the main builddir),
QEMU with `-s -S` on :1234, batch GDB with panic-capture semantics,
and serial command files for the interactive shell.

Usage:
  gdb_run.py debug-test  --arch A --build B --class C --gdb-script G [--gdb-port P]
  gdb_run.py debug-shell --arch A --build B --gdb-script G --cmds F [--gdb-port P]
  gdb_run.py rr-record   --arch A
  gdb_run.py rr-replay   --arch A [--gdb-port P]

GDB stub port (--gdb-port, default 1234): the requested port is used when
free; when busy a free ephemeral port is auto-picked (NOTE printed).
Default behavior is unchanged when 1234 is free.
"""

import argparse
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import qemu_test as harness  # noqa: E402


def find_gdb(arch):
    """Locate the arch GDB (triplet first, multiarch fallback)."""
    triplets = {"x86_64": "x86_64-elf-gdb",
                "aarch64": "aarch64-elf-gdb",
                "riscv64": "riscv64-elf-gdb"}
    for name in (triplets[arch], "gdb-multiarch", "gdb"):
        path = shutil.which(name)
        if path:
            return path
    print("ERROR: no GDB found (tried %s, gdb-multiarch, gdb)"
          % triplets[arch])
    return None


def ensure_nolto_builddir(arch, build):
    """Configure (once) and build the NO_LTO twin builddir."""
    builddir = Path("build/meson-%s-nolto" % arch).resolve()
    if not (builddir / "build.ninja").exists():
        subprocess.run(
            ["meson", "setup", str(builddir), "--cross-file",
             "cross/%s-elf.txt" % arch, "-Darch=%s" % arch,
             "-Db_lto=false"],
            cwd=str(HERE.parent.parent), check=True)
    return builddir


def build_nolto(arch, build, test_class):
    """Configure the test class and compile the NO_LTO tree.

    Bake via the meson test_class option (the test_config custom_target
    regenerates src/test-config.txt from it at build time) and assert the
    bake, mirroring qemu_test.py run_attempt. The old standalone
    gen_test_config.py call only wrote dead copies (builddir/test-config.txt
    has no consumer; the repo compat copy is overwritten by stage_initrd)
    while the custom_target reverted src/test-config.txt to the stale
    option — every debug-test run booted the wrong class (#419).
    """
    builddir = ensure_nolto_builddir(arch, build)
    subprocess.run(
        ["meson", "configure", "-Dtest_class=%s" % test_class, str(builddir)],
        check=True)
    subprocess.run(["meson", "compile", "-C", str(builddir)], check=True)
    harness.assert_baked(arch, builddir, test_class.split(","))
    return builddir


def wait_for_stub(port, timeout_secs=8):
    """Wait until something listens on 127.0.0.1:port."""
    import socket
    for _ in range(timeout_secs):
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=1):
                return True
        except OSError:
            time.sleep(1)
    return False


def port_value(text):
    """argparse type: integer in the valid TCP port range."""
    import argparse
    try:
        value = int(text)
    except ValueError:
        raise argparse.ArgumentTypeError("port must be an integer, got %r"
                                         % text)
    if not 1 <= value <= 65535:
        raise argparse.ArgumentTypeError("port must be 1-65535, got %d"
                                         % value)
    return value


def pick_gdb_port(requested):
    """Return (port, auto_picked): requested when free, else an ephemeral one.

    NOTE: bind-probe-then-release has a benign TOCTOU race — if the picked
    port is grabbed before QEMU binds it, QEMU fails loudly (no silent
    misdirection to another stub).
    """
    import socket
    probe = socket.socket()
    try:
        probe.bind(("127.0.0.1", requested))
        return requested, False
    except OSError:
        pass
    finally:
        probe.close()
    free = socket.socket()
    try:
        free.bind(("127.0.0.1", 0))
        return free.getsockname()[1], True
    finally:
        free.close()


def gdb_stub_argv(port):
    """QEMU argv fragment for the GDB stub. `-s` is QEMU shorthand for
    `-gdb tcp::1234`, so the default port keeps byte-identical argv."""
    if port == 1234:
        return ["-s", "-S"]
    return ["-gdb", "tcp::%d" % port, "-S"]


def gdb_script_for_port(script_path, port, logdir, tag):
    """Return a GDB script path whose `target ... :1234` line uses port.

    Default port: the caller's script itself (behavior unchanged).
    Otherwise a temp copy with the target port substituted, so batch GDB
    connects to the stub QEMU actually opened.
    """
    if port == 1234:
        return script_path
    import re
    text = Path(script_path).read_text()
    patched, n = re.subn(r"(target\s+\S+\s+:?)1234\b", r"\g<1>%d" % port,
                         text)
    if n == 0:
        # Flailing closed on purpose: a script that cannot be pointed at the
        # stub QEMU actually opened would silently target a foreign stub.
        print("ERROR: GDB script %s has no `target ... :1234` line; refusing "
              "to run against the auto-picked port :%d (fix the script or "
              "pin --gdb-port 1234)" % (script_path, port))
        raise SystemExit(2)
    out = Path(logdir) / ("gdb-script-%s-%d.gdb" % (tag, port))
    out.write_text(patched)
    print("NOTE: GDB script target port rewritten :1234 -> :%d (%s)"
          % (port, out))
    return str(out)


def launch_qemu(qemu_cmd, serial_log):
    """Start QEMU detached (serial to file, stdio to devnull)."""
    Path(serial_log).parent.mkdir(parents=True, exist_ok=True)
    try:
        Path(serial_log).unlink()
    except OSError:
        pass
    proc = subprocess.Popen(
        qemu_cmd, stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
        start_new_session=True)
    return proc


def cmd_debug_test(args):
    """Batch GDB surveillance run (mirrors `make debug-test`)."""
    if not Path(args.gdb_script).exists():
        print("ERROR: GDB script not found: %s" % args.gdb_script)
        return 2
    if args.cls == "none":
        requested = ["none"]
    else:
        requested = harness.resolve_classes(args.arch, args.build, args.cls)
    builddir = build_nolto(args.arch, args.build, ",".join(requested))
    tag = harness.checkout_tag()
    logdir = Path(tempfile.mkdtemp(prefix="jarvis-meson-gdb-"))
    paths = {
        "serial": str(logdir / "gdb-serial.log"),
        "com1": str(logdir / "gdb-com1.log"),
        "kernel_elf": str(builddir / "src" / "kernel-debug.elf"),
        "kernel_bin": str(builddir / "src" / "kernel-riscv64.bin"),
        "dtb": str(builddir / "build" / "virt.dtb"),
        "fat32": str(builddir / "packaging" / "fat32.img"),
        "iso": "",
        "scratch": str(logdir / "ahci_scratch.img"),
        "verdict": str(logdir / "verdict.log"),
        "start_ts": str(logdir / "start.timestamp"),
        "end_ts": str(logdir / "end.timestamp"),
    }
    if args.arch == "x86_64":
        iso = builddir / "src" / "nexios-rtos.iso"
        if not iso.exists():
            print("ERROR: no built ISO at %s" % iso)
            return 1
        paths = dict(paths, iso=iso)
    qemu_cmd = harness.qemu_argv(args.arch, args.build, args.cls, paths, tag)
    port, auto_picked = pick_gdb_port(args.gdb_port)
    if auto_picked:
        print("NOTE: :%d busy, auto-picked free GDB stub port :%d"
              % (args.gdb_port, port))
    gdb_script = gdb_script_for_port(args.gdb_script, port, logdir, "test")
    qemu_cmd = qemu_cmd + gdb_stub_argv(port) + ["-serial",
                                                 "file:%s" % paths["serial"]]
    print("  %-7s %s" % ("QEMU", "Starting with GDB stub (:%d)..." % port))
    proc = launch_qemu(qemu_cmd, paths["serial"])
    try:
        print("Waiting for QEMU GDB stub (:%d)..." % port)
        if not wait_for_stub(port):
            print("ERROR: GDB stub never appeared")
            return 1
        gdb = find_gdb(args.arch)
        if gdb is None:
            return 2
        print("  %-7s %s" % ("GDB", "Running %s..." % gdb_script))
        try:
            result = subprocess.run(
                [gdb, str(builddir / "src" / "kernel-debug.elf"),
                 "-batch", "-x", gdb_script],
                timeout=120)
            gdb_rc = result.returncode
        except subprocess.TimeoutExpired:
            gdb_rc = 124
        panic_marker = Path("build/gdb-panic-captured")
        if panic_marker.exists():
            print("")
            print("!!! KERNEL PANIC CAPTURED !!!")
            print("See GDB output above.")
            try:
                panic_marker.unlink()
            except OSError:
                pass
            return 1
        if gdb_rc == 124:
            print("")
            print("  %-7s %s" % ("GDB",
                                 "No panic within 120s (clean boot to shell)"))
            return 0
        if gdb_rc == 0:
            print("")
            print("  %-7s %s" % ("GDB", "No panic (QEMU exited cleanly)"))
            return 0
        print("")
        print("=== debug-test: GDB exit code %d (log: %s) ==="
              % (gdb_rc, paths["serial"]))
        return gdb_rc
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
        except (OSError, ProcessLookupError):
            pass
        harness.scoped_pkill(harness.QEMU_SYSTEM[args.arch], tag)


def cmd_debug_shell(args):
    """Interactive shell under GDB (mirrors `make debug-shell`)."""
    if args.cls != "none":
        print("ERROR: debug-shell requires class=none")
        return 2
    for path, label in ((args.gdb_script, "GDB script"),
                        (args.cmds, "shell commands file")):
        if not Path(path).exists():
            print("ERROR: %s not found: %s" % (label, path))
            return 2
    builddir = build_nolto(args.arch, args.build, "none")
    tag = harness.checkout_tag()
    logdir = Path(tempfile.mkdtemp(prefix="jarvis-meson-gdbsh-"))
    paths = {
        "serial": str(logdir / "gdb-serial.log"),
        "com1": str(logdir / "gdb-com1.log"),
        "kernel_elf": str(builddir / "src" / "kernel-debug.elf"),
        "kernel_bin": str(builddir / "src" / "kernel-riscv64.bin"),
        "dtb": str(builddir / "build" / "virt.dtb"),
        "fat32": str(builddir / "packaging" / "fat32.img"),
        "iso": "",
        "scratch": str(logdir / "ahci_scratch.img"),
        "verdict": str(logdir / "verdict.log"),
        "start_ts": str(logdir / "start.timestamp"),
        "end_ts": str(logdir / "end.timestamp"),
    }
    if args.arch == "x86_64":
        iso = builddir / "src" / "nexios-rtos.iso"
        if not iso.exists():
            print("ERROR: no built ISO at %s" % iso)
            return 1
        paths = dict(paths, iso=iso)
    qemu_cmd = harness.qemu_argv(args.arch, "debug", "none", paths, tag)
    port, auto_picked = pick_gdb_port(args.gdb_port)
    if auto_picked:
        print("NOTE: :%d busy, auto-picked free GDB stub port :%d"
              % (args.gdb_port, port))
    gdb_script = gdb_script_for_port(args.gdb_script, port, logdir, "shell")
    qemu_cmd = qemu_cmd + gdb_stub_argv(port)
    gdb = find_gdb(args.arch)
    if gdb is None:
        return 2
    print("  %-7s %s" % ("SHELL", "Starting debug session (%s, GDB stub :%d)..."
                          % (args.cmds, port)))
    script = (
        "fconfigure stdout -buffering none;\n"
        "set timeout 120;\n"
        "spawn {qemu};\n"
        "for {{set i 0}} {{$i < 8}} {{incr i}} {{\n"
        "  if {{[catch {{exec nc -z localhost {port}}}] == 0}} {{break}};\n"
        "  after 1000;\n"
        "}};\n"
        "exec {gdb} {elf} -batch -x {gdb_script} &;\n"
        "expect {{\n"
        "  -re {{jarvis\\$ }} {{ }};\n"
        "  timeout {{ puts \"TIMEOUT: shell prompt not reached\"; exit 1 }};\n"
        "}};\n"
        "set f [open {{{cmds}}} r];\n"
        "while {{[gets $f line] >= 0}} {{\n"
        "  puts \">>> $line\";\n"
        "  send \"$line\\r\";\n"
        "  expect {{\n"
        "    -re {{jarvis\\$ }} {{ }};\n"
        "    timeout {{ puts \"TIMEOUT after: $line\"; exit 1 }};\n"
        "  }};\n"
        "}};\n"
        "close $f;\n"
        "puts \"=== DEBUG SHELL COMPLETE ===\";\n"
        "exit 0;\n"
    ).format(qemu=" ".join(qemu_cmd), gdb=gdb,
             elf=str(builddir / "src" / "kernel-debug.elf"),
             gdb_script=gdb_script, cmds=args.cmds, port=port)
    try:
        result = subprocess.run(["expect", "-c", script])
        return result.returncode
    finally:
        harness.scoped_pkill(harness.QEMU_SYSTEM[args.arch], tag)


def cmd_rr(args, record):
    """Deterministic record/replay (mirrors rr-record / rr-replay)."""
    builddir = build_nolto(args.arch, args.build, "none")
    rr_log = Path("build/rr.log")
    if not record and not rr_log.exists():
        print("No recording found — run 'rr-record' first.")
        return 1
    tag = harness.checkout_tag()
    logdir = Path(tempfile.mkdtemp(prefix="jarvis-meson-rr-"))
    paths = {
        "serial": str(logdir / "rr-serial.log"),
        "com1": str(logdir / "rr-com1.log"),
        "kernel_elf": str(builddir / "src" / "kernel-debug.elf"),
        "kernel_bin": str(builddir / "src" / "kernel-riscv64.bin"),
        "dtb": str(builddir / "build" / "virt.dtb"),
        "fat32": str(builddir / "packaging" / "fat32.img"),
        "iso": "",
        "scratch": str(logdir / "ahci_scratch.img"),
        "verdict": str(logdir / "verdict.log"),
        "start_ts": str(logdir / "start.timestamp"),
        "end_ts": str(logdir / "end.timestamp"),
    }
    if args.arch == "x86_64":
        iso = builddir / "src" / "nexios-rtos.iso"
        if not iso.exists():
            print("ERROR: no built ISO at %s" % iso)
            return 1
        paths = dict(paths, iso=iso)
    qemu_cmd = harness.qemu_argv(args.arch, args.build, "none", paths, tag)
    mode = "record" if record else "replay"
    print("  %-7s %s" % ("RR", "Deterministic %s..." % ("trace" if record else "replay with GDB stub on :%d..." % args.gdb_port)))
    if not record:
        print("Connect GDB: gdb %s -ex 'target remote :%d'"
              % (builddir / "src" / "kernel-debug.elf", args.gdb_port))
        qemu_cmd = qemu_cmd + gdb_stub_argv(args.gdb_port)
    subprocess.run(qemu_cmd +
                   ["-icount", "shift=auto,rr=%s,rrfile=%s" % (mode, rr_log)])
    return 0


def main():
    """CLI entry point."""
    parser = argparse.ArgumentParser(description="GDB/record flows (Meson).")
    parser.add_argument("mode", choices=["debug-test", "debug-shell",
                                         "rr-record", "rr-replay"])
    parser.add_argument("--arch", required=True,
                        choices=["x86_64", "aarch64", "riscv64"])
    parser.add_argument("--build", default="debug", choices=["debug", "release"])
    parser.add_argument("--class", dest="cls", default="none")
    parser.add_argument("--gdb-script", default="")
    parser.add_argument("--cmds", default="")
    parser.add_argument("--gdb-port", type=port_value, default=1234,
                        help="GDB stub port (default 1234; free ephemeral "
                             "port auto-picked when busy)")
    args = parser.parse_args()
    if args.mode == "debug-test":
        if not args.gdb_script:
            print("debug-test needs --gdb-script")
            return 2
        return cmd_debug_test(args)
    if args.mode == "debug-shell":
        return cmd_debug_shell(args)
    return cmd_rr(args, record=args.mode == "rr-record")


if __name__ == "__main__":
    sys.exit(main())
