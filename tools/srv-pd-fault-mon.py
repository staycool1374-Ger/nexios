#!/usr/bin/env python3
"""srv_pd fault drill (issues #47/#48, strangler step c).

Boots an ON+fault-inject ISO, runs a parity probe, kills the PD via the
diagnostic drill command, waits for supervisor restart + rebind, runs a
second probe pass, and asserts no "admission denied" wedge.

Usage:
    tools/srv-pd-fault-mon.py --iso ISO [--probe PROBE.elf | --probe-cmd CMD]
        --tag b"S VFS " --role ROLE --kill-cmd CMDTYPE --outdir DIR
        [--ovmf PATH] [--fat32 PATH] [--port N]

CMDTYPE is one of: vfsfault | blockfault | termfault | logfault |
driverfault:<role>. --probe-cmd sends a shell command emitting tag
lines (e.g. logprobe) instead of the loadelf/runelf probe flow.
Artifact: outdir/{com1.log,debugcon.log,verdict.txt} (raw serial logs).
"""
import argparse
import importlib.util
import os
import sys

_parity_path = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                            "srv-pd-parity.py")
_spec = importlib.util.spec_from_file_location("srv_pd_parity", _parity_path)
if _spec is None or _spec.loader is None:
    raise SystemExit("ERROR: cannot load %s" % _parity_path)
_mod = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_mod)
find_ovmf, sendkey, wait_for = (_mod.find_ovmf, _mod.sendkey, _mod.wait_for)


def parse_args(argv=None):
    import argparse as ap
    p = ap.ArgumentParser(description=__doc__)
    p.add_argument("--iso", required=True)
    p.add_argument("--probe", default=None,
                   help="initrd probe path, e.g. /bin/vfs-srv-parity.c.elf")
    p.add_argument("--probe-cmd", default=None,
                   help="shell command emitting tag lines, e.g. logprobe "
                        "(alternative to --probe loadelf/runelf flow)")
    p.add_argument("--tag", required=True,
                   help="S-line tag prefix, e.g. 'S VFS '")
    p.add_argument("--role", required=True,
                   help="SDF role name for the kill command")
    p.add_argument("--kill-cmd", required=True,
                   help="vfsfault | blockfault | termfault | logfault | "
                        "driverfault:<role>")
    p.add_argument("--outdir", required=True)
    p.add_argument("--ovmf", default=None)
    p.add_argument("--fat32", default=None)
    p.add_argument("--port", type=int, default=4482)
    return p.parse_args(argv)


def count_tag(path, tag):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return -1, b""
    # NOTE: the first probe line shares its serial line with the echoed
    # shell prompt, so match anywhere and cut at the tag (mirrors
    # srv-pd-parity.py); startswith would drop the first line per pass.
    lines = [ln[ln.find(tag):].rstrip(b"\r")
             for ln in data.split(b"\n") if tag in ln]
    return len(lines), data


def main(argv=None):
    import socket
    import subprocess
    import time
    args = parse_args(argv)
    if (args.probe is None) == (args.probe_cmd is None):
        print("FAULT: need exactly one of --probe / --probe-cmd")
        return 2
    os.makedirs(args.outdir, exist_ok=True)
    com1 = os.path.join(args.outdir, "com1.log")
    dbg = os.path.join(args.outdir, "debugcon.log")
    ovmf = find_ovmf(args.ovmf)
    Dialect = {"vfsfault": "vfsfault", "blockfault": "blockfault",
               "termfault": "termfault", "logfault": "logfault"}
    if args.kill_cmd in Dialect:
        kill_line = Dialect[args.kill_cmd]
    elif args.kill_cmd.startswith("driverfault:"):
        kill_line = "driverfault %s" % args.kill_cmd.split(":", 1)[1]
    else:
        print("FAULT: unknown --kill-cmd %s" % args.kill_cmd)
        return 2
    qemu = subprocess.Popen(
        ["qemu-system-x86_64", "-cdrom", args.iso, "-m", "256M",
         "-chardev", "file,path=%s,id=dbg" % dbg,
         "-serial", "file:%s" % com1,
         "-device", "isa-debugcon,chardev=dbg",
         "-monitor", "tcp:127.0.0.1:%d,server,nowait" % args.port,
         "-display", "none",
         "-boot", "order=d", "-cpu", "max",
         "-drive", "if=pflash,format=raw,readonly=on,file=%s" % ovmf]
        + (["-drive", "file=%s,format=raw,if=ide,index=1,media=disk"
             % args.fat32] if args.fat32 else [])
        + ["-device", "virtio-net-pci,netdev=net0,disable-legacy=on",
           "-netdev", "user,id=net0",
           "-device", "virtio-rng-pci,disable-legacy=on",
           "-device", "isa-debug-exit,iobase=0xf4,iosize=0x04"],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    msock = None
    try:
        time.sleep(5)
        msock = socket.create_connection(("127.0.0.1", args.port),
                                         timeout=10)
        mf = msock.makefile("rw", buffering=1)
        mf.readline()
        if not wait_for(com1, b"$ ", 180):
            print("FAULT: FAIL no shell prompt; see %s" % com1)
            return 1
        print("FAULT: prompt seen", flush=True)

        def probe_pass():
            if args.probe_cmd is not None:
                sendkey(mf, args.probe_cmd)
                # wait_for matches the STALE (pre-command) prompt already
                # in the file, so sleep first: the shell runs the command
                # in ms; 8 s is TCG headroom (mirrors runelf's 15 s).
                time.sleep(8)
                if not wait_for(com1, b"$ ", 60):
                    print("FAULT: FAIL no prompt after probe-cmd",
                          flush=True)
                    return False
                return True
            sendkey(mf, "loadelf %s" % args.probe)
            if not wait_for(com1, b"$ ", 60):
                print("FAULT: FAIL no prompt after loadelf", flush=True)
                return False
            sendkey(mf, "runelf")
            time.sleep(15)
            return True

        if not probe_pass():
            return 1
        n1, data = count_tag(com1, args.tag.encode())
        print("FAULT: first run %s lines=%d" % (args.tag.strip(), n1),
              flush=True)
        sendkey(mf, kill_line)
        time.sleep(5)
        n2, data = count_tag(com1, args.tag.encode())
        if not probe_pass():
            return 1
        n3, data = count_tag(com1, args.tag.encode())
        denied = b"admission denied" in data
        ok = (n3 > n2) and not denied
        with open(os.path.join(args.outdir, "verdict.txt"), "w") as f:
            f.write("before=%d after=%d denied=%s\n"
                    % (n2, n3, denied))
        print("FAULT: before=%d after=%d denied=%s" % (n2, n3, denied),
              flush=True)
        if ok:
            print("FAULT-DEMO-OK: restart + rebind + second run, "
                  "no admission wedge", flush=True)
            return 0
        print("FAULT: FAIL (no second-run S-lines or admission wedge)",
              flush=True)
        return 1
    finally:
        if msock is not None:
            try:
                msock.close()
            except Exception:
                pass
        qemu.kill()
        qemu.wait()


if __name__ == "__main__":
    sys.exit(main())

