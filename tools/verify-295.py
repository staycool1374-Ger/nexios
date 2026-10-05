#!/usr/bin/env python3
"""Live E2E proof for #295: shell launches --debug target, RSP drives it.

Spawns QEMU itself on a pty (mon:stdio, like run-debug-mode) and drives
the shell commands AND the RSP session over the single line
(sequentially, so no contention): loadelf hey, runelf --debug, then RSP
?/g/m/c/D with checksum validation.

Usage: python3 tools/verify-295.py  (from repo root, ISO already built)
"""
import os
import pty
import select
import subprocess
import sys
import termios
import time
import tty

PASS = 0
FAIL = 0


def check(cond, label):
    global PASS, FAIL
    if cond:
        PASS += 1
        print("VERIFY: %s" % label, flush=True)
    else:
        FAIL += 1
        print("VERIFY-FAIL: %s" % label, flush=True)


QEMU_ARGS = [
    "qemu-system-x86_64",
    "-cdrom", "debug/nexios-rtos.iso",
    "-m", "256M",
    "-serial", "mon:stdio",
    "-netdev", "user,id=net0",
    "-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
    "-boot", "order=d",
    "-drive", "if=pflash,format=raw,readonly=on,file=/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
    "-cpu", "max",
    "-drive", "file=build/fat32.img,format=raw,if=ide,index=1,media=disk",
    "-display", "none",
    "-no-reboot",
]

MASTER = -1


def _fd():
    assert MASTER >= 0
    return MASTER


def recv_until(marker, timeout_s=30):
    buf = b""
    end = time.time() + timeout_s
    while time.time() < end:
        r, _, _ = select.select([_fd()], [], [], 0.5)
        if r:
            try:
                chunk = os.read(_fd(), 4096)
            except OSError:
                continue
            if chunk:
                buf += chunk
            if marker in buf:
                return buf
    return buf


def rsp_checksum(data):
    return sum(data) & 0xFF


def send_packet(body):
    pkt = b"$" + body + b"#%02x" % rsp_checksum(body)
    os.write(_fd(), pkt)
    ack = recv_until(b"+", timeout_s=10)
    return b"+" in ack


def read_packet(timeout_s=15):
    """Read one $...#cs packet body (ACKs it). Returns body or None."""
    end = time.time() + timeout_s
    # Find '$' (skip ACKs, Ctrl-C handled by debugd, junk).
    while time.time() < end:
        r, _, _ = select.select([_fd()], [], [], 0.5)
        if not r:
            continue
        try:
            chunk = os.read(_fd(), 1)
        except OSError:
            continue
        if not chunk:
            continue
        if chunk == b"$":
            break
    else:
        return None
    buf = b""
    while time.time() < end:
        r, _, _ = select.select([_fd()], [], [], 0.5)
        if not r:
            continue
        try:
            chunk = os.read(_fd(), 1)
        except OSError:
            continue
        if not chunk:
            continue
        if chunk == b"#":
            break
        buf += chunk
    else:
        return None
    csum = b""
    while len(csum) < 2 and time.time() < end:
        r, _, _ = select.select([_fd()], [], [], 0.5)
        if not r:
            continue
        try:
            chunk = os.read(_fd(), 1)
        except OSError:
            continue
        if chunk:
            csum += chunk
    if len(csum) < 2:
        return None
    try:
        want = int(csum, 16)
    except ValueError:
        return None
    if want != rsp_checksum(buf):
        os.write(_fd(), b"-")
        return None
    os.write(_fd(), b"+")
    return buf


def main():
    global MASTER
    # Environment hygiene: a stray QEMU holding build/fat32.img makes a
    # fresh instance die instantly and silently (error goes to DEVNULL).
    # Only our own image path is matched.
    try:
        subprocess.run(["pkill", "-9", "-f",
                        "qemu-system-x86_64.*nexios-rtos"],
                       capture_output=True, timeout=10)
    except Exception:
        pass
    time.sleep(2)
    master, slave = pty.openpty()
    tty.setraw(slave)
    try:
        tty.setraw(slave, termios.TCSANOW)
    except Exception:
        pass
    MASTER = master
    qemu = subprocess.Popen(
        QEMU_ARGS, stdin=slave, stdout=slave, stderr=subprocess.DEVNULL,
        close_fds=True)
    os.close(slave)
    # Spawn guard: a QEMU that dies instantly (e.g. fat32 image lock held
    # by a stray process — its error goes to DEVNULL) would otherwise
    # cost 13 full timeouts. Fail fast with a diagnosis instead.
    for _ in range(30):
        time.sleep(2)
        if qemu.poll() is not None:
            print("VERIFY-FAIL: QEMU died at startup (rc=%s); suspect a "
                  "stray qemu holding build/fat32.img" % (qemu.returncode,),
                  flush=True)
            return 2
        r, _, _ = select.select([_fd()], [], [], 0.1)
        if r:
            break
    else:
        print("VERIFY-FAIL: no serial output 60s after spawn; QEMU stuck?",
              flush=True)
        try:
            qemu.kill()
        except Exception:
            pass
        return 2
    try:
        # Shell prompt (banner + boot first; slow under TCG load).
        out = recv_until(b"$ ", timeout_s=400)
        check(b"$ " in out, "shell prompt reached")
        # Load + launch under debug.
        os.write(_fd(), b"loadelf hey.c.elf\r")
        out = recv_until(b"completed successfully", timeout_s=60)
        check(b"completed successfully" in out, "hey.c.elf loaded")
        os.write(_fd(), b"runelf --debug\r")
        out = recv_until(b"Started task #", timeout_s=60)
        check(b"Started task #" in out, "debug target started")
        time.sleep(2)
        # RSP: stop reason. Target holds at the entry breakpoint.
        check(send_packet(b"?"), "? acked")
        body = read_packet()
        check(body is not None and (body.startswith(b"T05") or
                                    body.startswith(b"T0b")),
              "stop reason T05/T0b, got %r" % (body,))
        # RSP: registers (164-byte GDB-order blob = 328 hex chars).
        check(send_packet(b"g"), "g acked")
        body = read_packet()
        check(body is not None and len(body) == 328,
              "g blob 328 hex, got %r" %
              (len(body) if body is not None else None,))
        # RSP: read entry memory (16 bytes at 0x400000).
        check(send_packet(b"m400000,10"), "m acked")
        body = read_packet()
        check(body is not None and len(body) == 32,
              "m returns 16 bytes, got %r" % (body,))
        # RSP: continue then immediate Ctrl-C stop injection.
        check(send_packet(b"c"), "c acked (async, no reply)")
        time.sleep(1)
        os.write(_fd(), b"\x03")
        body = read_packet(timeout_s=20)
        check(body is not None and body.startswith(b"T"),
              "ctrl-c stop injected, got %r" % (body,))
        # RSP: detach cleanly.
        check(send_packet(b"D"), "D acked")
        body = read_packet()
        check(body == b"OK", "detach OK, got %r" % (body,))
        # Live burner: silent long-running user load for real stats.
        os.write(_fd(), b"loadelf burn.c.elf\r")
        out = recv_until(b"completed successfully", timeout_s=120)
        check(b"completed successfully" in out, "burn.c.elf loaded")
        os.write(_fd(), b"runelf\r")
        out = recv_until(b"Started task #", timeout_s=60)
        check(b"Started task #" in out, "burner started")
        time.sleep(5)
        os.write(_fd(), b"monstat user\r")
        out = recv_until(b"exec_ns=", timeout_s=60)
        check(b"exec_ns=" in out, "live burner has exec stats")
        # Post-mortem: poll until the burner exits, then its record.
        print("VERIFY: waiting for burner exit (post-mortem)...", flush=True)
        found = False
        for _ in range(30):
            time.sleep(20)
            os.write(_fd(), b"monstat user\r")
            out = recv_until(b"$ ", timeout_s=30)
            if b"POST-MORTEM" in out:
                found = True
                break
        check(found, "burner post-mortem appeared")
        if found:
            check(b"exit=0" in out, "post-mortem exit code 0")
        print("VERIFY: %d passed, %d failed" % (PASS, FAIL), flush=True)
        return 1 if FAIL else 0
    finally:
        try:
            qemu.kill()
        except Exception:
            pass
        os.close(_fd())


if __name__ == "__main__":
    sys.exit(main())
