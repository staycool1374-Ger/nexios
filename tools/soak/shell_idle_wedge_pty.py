#!/usr/bin/env python3
"""Soak: x86_64 interactive shell idle-wedge probe (M2 QE, issues #545/#546).

Repro shape from #545: after a few commands + ~1 min idle, the serial
shell goes dead (no echo, no exec) with QEMU alive. Pipe-stdin never
reproduces it — the guest serial must sit on a PTY slave (faithful
macOS-TTY path), which is what this script does.

Per run: boot the test_class=none ISO, script version/help/loadelf
hey.c.elf/runelf, idle IDLE_S doing nothing, then probe with a bare
newline and require the next shell prompt within PROBE_S. A missed probe
is re-probed once (TCG jitter must not fail the soak); a persistent miss
is a wedge: serial capture + `info registers` via the dedicated monitor
socket are saved and the run fails.

Adopted permanently (testbed-owned; replaces the /tmp repro harness
referenced on #545, which was never homed in-tree).

Usage:
  python3 tools/soak/shell_idle_wedge_pty.py [--runs 5] [--idle 90]
                                             [--smoke] [--keep-iso-class]

--smoke shortens to 1 run x 10 s idle (harness validation only).
The script reconfigures build/meson-x86_64 to -Dtest_class=none and
rebuilds; the next qemu_test.py run re-bakes its own class automatically,
so no restore is needed.
"""

import argparse
import os
import pty
import select
import socket
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "meson"))
import qemu_test as harness  # noqa: E402  (argv builder + OVMF locate)

BUILD_DIR = ROOT / "build" / "meson-x86_64"
PROMPT = b"$ "
# Shell-smoke discipline (tools/shell-smoke.exp, issue #245): a bare
# re-prompt proves NOTHING (async background output redraws the prompt,
# faking a round-trip while typed bytes rot in the FIFO). Every scripted
# command needs its real output needle. The shell takes CR as Enter.
CR = "\r"
VERSION_NEEDLE = b"Kernel: "
RUNELF_NEEDLE = b"Hey!"


def run(cmd, **kw):
    print("+ %s" % " ".join(str(c) for c in cmd), flush=True)
    subprocess.run(cmd, check=True, cwd=str(ROOT), **kw)


def ensure_none_iso():
    run(["meson", "configure", str(BUILD_DIR), "-Dtest_class=none"])
    run(["meson", "compile", "-C", str(BUILD_DIR)])
    iso = BUILD_DIR / "src" / "nexios-rtos.iso"
    if not iso.exists():
        sys.exit("ERROR: missing built ISO %s" % iso)
    return iso


def build_argv(mon_sock, logdir):
    tag = harness.checkout_tag()
    paths = {
        "serial": str(logdir / "debugcon.log"),
        "com1": str(logdir / "com1.log"),
        "iso": str(BUILD_DIR / "src" / "nexios-rtos.iso"),
        "fat32": str(BUILD_DIR / "packaging" / "fat32.img"),
    }
    argv = harness.qemu_argv("x86_64", "debug", "none", paths, tag)
    out = []
    skip_next = False
    for tok in argv:
        if skip_next:
            skip_next = False
            continue
        if tok == "-serial":
            out += ["-serial", "mon:stdio"]
            skip_next = True
        elif tok == "-monitor":
            out += ["-monitor", "unix:%s,server,nowait" % mon_sock]
            skip_next = True
        else:
            out.append(tok)
    for banned in ("-s", "-S", "-gdb"):
        if banned in out:
            sys.exit("internal error: stub flag %s leaked" % banned)
    return out


class Guest:
    def __init__(self, argv, logpath):
        self.log = open(logpath, "wb")
        self.buf = b""
        # pty.fork (not Popen+slave): the slave becomes QEMU's controlling
        # terminal, mirroring the operator's repro harness. A bare
        # Popen stdio slave demonstrably delivers no input to the guest.
        self.pid, self.master = pty.fork()
        if self.pid == 0:
            os.execvp(argv[0], argv)

    def close(self):
        import signal
        try:
            os.kill(self.pid, signal.SIGTERM)
            _, _ = os.waitpid(self.pid, 0)
        except Exception:
            pass
        try:
            os.close(self.master)
        except OSError:
            pass
        self.log.close()

    def _drain(self, timeout):
        end = time.time() + timeout
        while True:
            wait = end - time.time()
            if wait <= 0:
                return
            r, _, _ = select.select([self.master], [], [], wait)
            if not r:
                return
            try:
                chunk = os.read(self.master, 65536)
            except OSError:
                return
            if not chunk:
                return
            self.buf += chunk
            self.log.write(chunk)
            self.log.flush()

    def expect(self, needle, timeout):
        end = time.time() + timeout
        while True:
            idx = self.buf.find(needle)
            if idx >= 0:
                del_note = self.buf[:idx + len(needle)]
                self.buf = self.buf[idx + len(needle):]
                return del_note
            if time.time() >= end:
                return None
            self._drain(end - time.time())

    def send(self, text):
        os.write(self.master, text.encode())


def monitor_info(mon_sock):
    try:
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.settimeout(10)
        s.connect(str(mon_sock))
        data = b""
        s.sendall(b"info registers\n")
        end = time.time() + 10
        while time.time() < end:
            try:
                chunk = s.recv(65536)
            except socket.timeout:
                break
            if not chunk:
                break
            data += chunk
            if b"(qemu)" in data:
                break
        s.close()
        return data
    except Exception as exc:  # noqa: BLE001 — forensics best-effort
        return ("monitor query failed: %s\n" % exc).encode()


def one_run(index, idle_s, probe_s, logdir):
    mon_sock = logdir / ("mon-%d.sock" % index)
    if mon_sock.exists():
        mon_sock.unlink()
    argv = build_argv(mon_sock, logdir)
    guest = Guest(argv, logdir / ("pty-%d.log" % index))
    try:
        if guest.expect(PROMPT, 120) is None:
            return (False, "no shell prompt within 120 s of boot")
        guest.send("version" + CR)
        if guest.expect(VERSION_NEEDLE, 15) is None:
            return (False, "no version output (input dead?)")
        if guest.expect(PROMPT, 15) is None:
            return (False, "no prompt back after version")
        guest.send("help" + CR)
        if guest.expect(PROMPT, 15) is None:
            return (False, "no prompt after 'help'")
        guest.send("loadelf hey.c.elf" + CR)
        if guest.expect(PROMPT, 30) is None:
            return (False, "no prompt after loadelf")
        time.sleep(2)
        guest.send("runelf" + CR)
        if guest.expect(RUNELF_NEEDLE, 60) is None:
            return (False, "no hey output after runelf")
        if guest.expect(PROMPT, 15) is None:
            return (False, "no prompt back after runelf")
        # Idle window: drain (so the wedge signature is absence of
        # solicited output, not unread backlog) but send nothing.
        guest._drain(idle_s)
        guest.send(CR)
        if guest.expect(PROMPT, probe_s) is None:
            time.sleep(5)
            guest.send(CR)
            if guest.expect(PROMPT, probe_s) is None:
                regs = monitor_info(mon_sock)
                (logdir / ("regs-%d.log" % index)).write_bytes(regs)
                return (False, "probe miss persisted after re-probe "
                               "(serial + regs saved)")
        return (True, "ok")
    finally:
        guest.close()
        try:
            if mon_sock.exists():
                mon_sock.unlink()
        except OSError:
            pass


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=5)
    ap.add_argument("--idle", type=float, default=90.0)
    ap.add_argument("--probe", type=float, default=2.0)
    ap.add_argument("--smoke", action="store_true",
                    help="1 run x 10 s idle (validation only)")
    args = ap.parse_args()
    if args.smoke:
        args.runs, args.idle = 1, 10.0
    logdir = BUILD_DIR / "soak-logs"
    logdir.mkdir(parents=True, exist_ok=True)
    ensure_none_iso()
    results = []
    for i in range(args.runs):
        ok, note = one_run(i, args.idle, args.probe, logdir)
        print("soak run %d/%d: %s (%s)"
              % (i + 1, args.runs, "PASS" if ok else "WEDGE", note),
              flush=True)
        results.append(ok)
    print("soak: %d/%d clean" % (sum(results), len(results)), flush=True)
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
