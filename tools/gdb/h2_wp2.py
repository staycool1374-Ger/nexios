#!/usr/bin/env python3
"""lldb hardware-watchpoint driver for the residual H2 race.
Watches the harness's TCB context.rsp field (write).  If it is EVER written
with a value outside the harness's kslot stack, that is the displacement save:
we stop and dump the write site, the live RSP, and the stack.

Run:  lldb -b -s h2_wp2.txt"""
import sys

import lldb

CPU_CTX = 0xFFFF8000004A9C60          # kernel::current_cpu()::cpu (CpuContext)
OFF_ID = 0x360
OFF_STATE = 0x370
OFF_CTX_RSP = 0x478
OFF_KST = 0x488
OFF_KST_TOP = 0x490
TICKS = 0xFFFF8000004A9C70            # CpuContext.ticks (+0x10)


def rd(tgt, addr):
    err = lldb.SBError()
    mem = tgt.GetProcess().ReadMemory(addr, 8, err)
    if err.Fail() or mem is None:
        return 0
    return int.from_bytes(bytes(mem), "little")


def dump_stack(tgt, rsp, nq=24):
    err = lldb.SBError()
    mem = tgt.GetProcess().ReadMemory(rsp, nq * 8, err)
    if err.Fail() or mem is None:
        print("  (stack unreadable at 0x%x)" % rsp)
        return
    for i in range(0, nq * 8, 8):
        print("  [rsp+%3d]=0x%x" % (i, int.from_bytes(bytes(mem[i:i+8]), "little")))


def main():
    dbg = lldb.debugger
    dbg.SetAsync(False)
    target = dbg.GetSelectedTarget()
    process = target.GetProcess()
    print("driver: connected state=%d" % process.GetState())
    sys.stdout.flush()

    bp = target.BreakpointCreateByName('rate_monotonic_schedule')
    if bp is None or bp.GetNumLocations() == 0:
        print("no tick symbol")
        return 4
    print("tick breakpoint id=%d" % bp.GetID())
    sys.stdout.flush()

    cur = 0
    for _ in range(6000):
        err = process.Continue()
        if err.Fail():
            print("continue error:", err.GetCString())
            return 2
        cur = rd(target, CPU_CTX)
        if cur == 0:
            continue
        if rd(target, cur + OFF_ID) == 1:
            break
    if cur == 0:
        print("harness not found")
        return 3
    kst = rd(target, cur + OFF_KST)
    ktop = rd(target, cur + OFF_KST_TOP)
    ctx_addr = cur + OFF_CTX_RSP
    print("harness TCB=0x%x ctx.rsp=0x%x kst=[0x%x-0x%x] tick=%d"
          % (cur, ctx_addr, kst, ktop, rd(target, TICKS)))
    sys.stdout.flush()

    # Watch context.rsp (write).  Report every write; stop on a foreign value.
    opts = lldb.SBWatchpointOptions()
    opts.SetWatchpointTypeRead(False)
    opts.SetWatchpointTypeWrite(True)
    werr = lldb.SBError()
    wp = target.WatchpointCreateByAddress(ctx_addr, 8, opts, werr)
    print("watchpoint id:", wp.GetID() if wp is not None else "none")
    sys.stdout.flush()
    if wp is None:
        print("watchpoint create failed:", werr.GetCString())
        return 5

    hits = 0
    while True:
        err = process.Continue()
        if err.Fail():
            print("continue error:", err.GetCString())
            return 2
        state = process.GetState()
        if state != lldb.eStateStopped:
            print("state=%d aborting" % state)
            return 3
        hits += 1
        val = rd(target, ctx_addr)
        foreign = (val < kst or val >= ktop)
        th = process.GetSelectedThread()
        f0 = th.GetFrameAtIndex(0)
        print("  wp#%d tick=%d ctx.rsp=0x%x %s rip=0x%x rsp=0x%x" % (
            hits, rd(target, TICKS), val, "FOREIGN!" if foreign else "ok",
            f0.GetPC(), f0.GetSP()))
        sys.stdout.flush()
        if foreign:
            print("=== FOREIGN context.rsp write (displacement save) ===")
            print("write-site rip=0x%x  live rsp=0x%x  tick=%d"
                  % (f0.GetPC(), f0.GetSP(), rd(target, TICKS)))
            for i in range(8):
                f = th.GetFrameAtIndex(i)
                if not f.IsValid():
                    break
                print("  #%d 0x%x %s" % (i, f.GetPC(), f.GetFunctionName() or "?"))
            print("--- stack at write site ---")
            dump_stack(target, f0.GetSP())
            print("--- ctx.rsp frame (stored) ---")
            dump_stack(target, val, 8)
            open("/tmp/lldb-h2w-foreign.txt", "w").write(
                "FOREIGN ctx.rsp=0x%x rip=0x%x live_rsp=0x%x tick=%d"
                % (val, f0.GetPC(), f0.GetSP(), rd(target, TICKS)))
            return 0
        if hits > 800:
            print("no foreign write in %d hits (tick=%d)" % (hits, rd(target, TICKS)))
            return 4


if __name__ == "__main__":
    sys.exit(main())
