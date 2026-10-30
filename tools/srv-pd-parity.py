#!/usr/bin/env python3
"""srv_pd parity harness (issues #47/#48, strangler step b).

Boots an ISO, drives the interactive shell over the QEMU monitor's
sendkey (harness-proven path: file-backed COM1 for the artifact, PS/2
keyboard for input), runs a parity probe ELF, and writes filtered S-lines.

Usage:
    tools/srv-pd-parity.py <iso> <probe-elf> <tag-pattern> <outdir>
        [--ovmf PATH] [--fat32 PATH] [--port N]

Artifact: outdir/{com1.log,debugcon.log,filtered.txt,verdict.txt}.
Requires QEMU with a pflash OVMF image for the El Torito ISO.
"""
import argparse
import os
import re
import socket
import subprocess
import sys
import time


def parse_args(argv=None):
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("iso")
    p.add_argument("probe_elf")
    p.add_argument("tag_pattern")
    p.add_argument("outdir")
    p.add_argument("--ovmf", default=None)
    p.add_argument("--fat32", default=None)
    p.add_argument("--port", type=int, default=4481)
    return p.parse_args(argv)


def find_ovmf(explicit):
    if explicit and os.path.isfile(explicit):
        return explicit
    for base in ("/opt/homebrew/Cellar/qemu", "/usr/share/qemu",
                 "/usr/local/share/qemu"):
        for root, _, files in os.walk(base):
            if "edk2-x86_64-code.fd" in files:
                return os.path.join(root, "edk2-x86_64-code.fd")
    raise SystemExit("ERROR: no OVMF edk2-x86_64-code.fd found")


def sendkey(mf, text):
    for ch in text:
        if ch == " ":
            key = "spc"
        elif ch == "/":
            key = "slash"
        elif ch == "-":
            key = "minus"
        elif ch == ".":
            key = "dot"
        else:
            key = ch
        mf.write("sendkey %s\n" % key)
        mf.flush()
        time.sleep(0.15)
    mf.write("sendkey ret\n")
    mf.flush()
    time.sleep(1.0)


def wait_for(path, needle, timeout):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(path, "rb") as f:
                if needle in f.read():
                    return True
        except OSError:
            pass
        time.sleep(2.0)
    return False


def main(argv=None):
    args = parse_args(argv)
    os.makedirs(args.outdir, exist_ok=True)
    com1 = os.path.join(args.outdir, "com1.log")
    dbg = os.path.join(args.outdir, "debugcon.log")
    ovmf = find_ovmf(args.ovmf)
    fat32 = args.fat32
    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", args.iso, "-m", "256M",
         "-chardev", "file,path=%s,id=dbg" % dbg,
         "-serial", "file:%s" % com1,
         "-device", "isa-debugcon,chardev=dbg",
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % args.port,
         "-display", "none",
         "-boot", "order=d", "-cpu", "max",
         "-drive", "if=pflash,format=raw,readonly=on,file=%s" % ovmf]
        + (["-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % fat32]
           if fat32 else [])
        + ["-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
           "-netdev", "user,id=net0",
         "-device", "virtio-rng-pci,disable-legacy=on",
         "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(5)
        msock = socket.create_connection(("127.0.0.1", args.port),
                                         timeout=10)
        mf = msock.makefile("rw", buffering=1)
        mf.readline()
        if not wait_for(com1, b"$ ", 180):
            print("PARITY: FAIL no shell prompt; see %s" % com1)
            return 1
        print("PARITY: prompt seen", flush=True)
        probe = args.probe_elf
        if probe.startswith("/bin/"):
            probe = probe
        sendkey(mf, "loadelf %s" % probe)
        if not wait_for(com1, b"$ ", 60):
            print("PARITY: FAIL no prompt after loadelf", flush=True)
            return 1
        sendkey(mf, "runelf")
        time.sleep(15)
        with open(com1, "rb") as f:
            data = f.read()
        pat = args.tag_pattern.encode()
        # NOTE: the first probe line shares its serial line with the
        # echoed shell prompt (`✓ / $ S ...`), so match anywhere and
        # cut at the tag (LEARNINGS: E2E serial is multiplexed).
        lines = []
        for ln in data.split(b"\n"):
            at = ln.find(pat)
            if at >= 0:
                lines.append(ln[at:].rstrip(b"\r"))
        filt = os.path.join(args.outdir, "filtered.txt")
        with open(filt, "wb") as f:
            f.write(b"\n".join(lines) + b"\n")
        with open(os.path.join(args.outdir, "verdict.txt"), "w") as f:
            f.write("S-lines: %d\ntag: %s\n" % (len(lines), args.tag_pattern))
        print("PARITY: %d %s lines -> %s" % (len(lines), args.tag_pattern,
                                             filt), flush=True)
        msock.close()
        return 0 if lines else 2
    finally:
        qemu.kill()
        qemu.wait()


if __name__ == "__main__":
    sys.exit(main())
