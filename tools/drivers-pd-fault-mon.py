#!/usr/bin/env python3
"""Driver PD fault drill via QEMU monitor sendkey (issue #48 step-b).

Boots an ON+FAULT_INJECT ISO with file-backed COM1 (harness-proven path;
TCP serial proved unreliable under TCG load here), types a parity run,
driverfault <role>, waits for supervisor restart + rebind, and re-runs.
Fails on any "admission denied" (restart reservation leak) or missing
second-run output. Usage: tools/drivers-pd-fault-mon.py (artifacts:
serial + debugcon logs under /tmp/nexios-48/fault-step-b/).
"""
import socket
import subprocess
import sys
import time

ISO = "build/meson-x86_64-drvpd/src/nexios-rtos.iso"
COM1 = "/tmp/nexios-48/fault-step-b/pd-com1.log"
DBG = "/tmp/nexios-48/fault-step-b/pd-fault-serial.log"
MPORT = 4479
OVMF = "/opt/homebrew/Cellar/qemu/11.0.1/share/qemu/edk2-x86_64-code.fd"
FAT = "build/meson-x86_64-drvpd/packaging/fat32.img"


def mon_cmd(mf, cmd, wait=1.0):
    mf.write(cmd + "\n")
    mf.flush()
    time.sleep(wait)
    out = []
    mfile = mf
    import select
    end = time.time() + 5
    while time.time() < end:
        r, _, _ = select.select([mfile], [], [], 1.0)
        if not r:
            break
        line = mfile.readline()
        if not line:
            break
        out.append(line.rstrip())
        if "(qemu)" in line and len(out) > 1:
            break
    return out


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
    time.sleep(1.5)


def com1_contains(needle, path=COM1):
    try:
        with open(path, "rb") as f:
            data = f.read()
        return needle in data
    except OSError:
        return False


qemu = subprocess.Popen(
    ["qemu-system-x86_64", "-cdrom", ISO, "-m", "256M",
     "-chardev", "file,path=%s,id=dbg" % DBG,
     "-serial", "file:%s" % COM1,
     "-device", "isa-debugcon,chardev=dbg",
     "-monitor", "tcp:127.0.0.1:%d,server,nowait" % MPORT,
     "-display", "none",
     "-boot", "order=d", "-cpu", "max",
     "-drive", "if=pflash,format=raw,readonly=on,file=%s" % OVMF,
     "-drive", "file=%s,format=raw,if=ide,index=1,media=disk" % FAT],
    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(5)
    m = socket.create_connection(("127.0.0.1", MPORT), timeout=10)
    mf = m.makefile("rw", buffering=1)
    mf.readline()
    print("MONITOR connected, waiting for shell prompt...", flush=True)
    ok = False
    for _ in range(24):
        time.sleep(10)
        if com1_contains(b"$ "):
            ok = True
            break
    if not ok:
        print("FAIL: no shell prompt in com1 log", flush=True)
        sys.exit(1)
    print("PROMPT seen", flush=True)
    sendkey(mf, "loadelf /bin/driver-pd-parity.c.elf")
    sendkey(mf, "runelf")
    time.sleep(10)
    sendkey(mf, "driverfault serial")
    time.sleep(12)
    sendkey(mf, "loadelf /bin/driver-pd-parity.c.elf")
    sendkey(mf, "runelf")
    time.sleep(15)
    with open(COM1, "rb") as f:
        data = f.read()
    ndrv = data.count(b"S DRV ")
    denied = b"admission denied" in data
    print("S DRV lines: %d, admission denied: %s" % (ndrv, denied),
          flush=True)
    if denied:
        print("FAULT-DEMO FAIL: admission wedged after restart", flush=True)
        sys.exit(1)
    if ndrv >= 10:
        print("FAULT-DEMO-OK: restart + rebind + second run green",
              flush=True)
    else:
        print("FAULT-DEMO UNCLEAR: S DRV=%d, inspect %s" % (ndrv, COM1),
              flush=True)
        sys.exit(2)
    m.close()
finally:
    qemu.kill()
    qemu.wait()
