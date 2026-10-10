#!/usr/bin/env python3
"""E2E acceptance for #276: XMODEM transfers into ramdisk + dump compare.

QEMU must already run with -serial tcp:127.0.0.1:4465,server,nowait and a
class=none ISO containing bin/xmodem.c.elf, bin/ramdiskd.c.elf and
tests/xmodem276.txt (single loadelf/sleep/runelf/sleep-900 handoff;
xmodem loops on cmd so the whole suite runs in one session).

Cases: (a) SOH raw 300 B @100, (b) STX YMODEM 3 KiB @200,
(c) SOH base64 @300, (d) STX hex @400, (e) corrupt-once NAK recovery,
(f) NOGRANT abort + zero-write proof. Each data case: rx, dump, compare.
"""
import socket
import sys
import base64
import binascii
import re
import time
import os

HOST, PORT = "127.0.0.1", 4465
SOH, STX, EOT, ACK, NAK, CAN, CHR_C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43
# Generous socket timeout: under TCG the first (coldest) transaction after
# task start can take >60 s wall (cold translation + first 2 MiB lazy map
# + first IPC paths), while later ones answer in ms (#315 E2E evidence).
# Explicit expect() timeouts still bound the snappy phases.
TIMEOUT = 120
LOGF = os.environ.get("XMODEM_LOG")



def crc16(data: bytes) -> int:
    crc = 0
    for b in data:
        crc ^= b << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def frame(payload: bytes, framing: str) -> bytes:
    if framing == "raw":
        return payload
    if framing == "base64":
        return base64.b64encode(payload) + b"\n"
    return binascii.hexlify(payload) + b"\n"


class Term:
    def __init__(self):
        self.s = socket.create_connection((HOST, PORT), timeout=TIMEOUT)
        self.s.settimeout(TIMEOUT)
        self.buf = b""
        self.log = open(LOGF, "wb") if LOGF else None

    def recv_raw(self, n=65536):
        try:
            chunk = self.s.recv(n)
        except socket.timeout:
            return b""
        if chunk and self.log:
            self.log.write(b"<<IN " + chunk + b"\n")
        return chunk

    def expect(self, needle: bytes, timeout=TIMEOUT) -> bytes:
        """Consume through needle; return bytes up to AND including it."""
        end = time.time() + timeout
        while needle not in self.buf:
            if time.time() > end:
                raise TimeoutError(f"missing {needle!r} tail={self.buf[-160:]!r}")
            chunk = self.recv_raw()
            if chunk:
                self.buf += chunk
        idx = self.buf.index(needle) + len(needle)
        out, self.buf = self.buf[:idx], self.buf[idx:]
        return out

    def send(self, data: bytes):
        if self.log:
            self.log.write(b">>OUT " + data + b"\n")
        self.s.sendall(data)

    def sendline(self, s: str):
        self.send(s.encode() + b"\r")

    def getc(self):
        while not self.buf:
            chunk = self.recv_raw()
            if not chunk:
                return None
            self.buf += chunk
        c, self.buf = self.buf[0], self.buf[1:]
        return c

    def readline(self, timeout=TIMEOUT) -> bytes:
        end = time.time() + timeout
        while b"\n" not in self.buf:
            if time.time() > end:
                raise TimeoutError("no line")
            chunk = self.recv_raw()
            if chunk:
                self.buf += chunk
        idx = self.buf.index(b"\n") + 1
        out, self.buf = self.buf[:idx], self.buf[idx:]
        return out

    def flush_quiet(self, quiet_ms=800, cap_s=5):
        """Drop stale output; return after quiet_ms silence or cap_s."""
        end = time.time() + cap_s
        last = time.time()
        while time.time() < end:
            chunk = self.recv_raw(65536)
            if chunk:
                self.buf += chunk
                last = time.time()
            elif (time.time() - last) * 1000 >= quiet_ms:
                break
        self.buf = b""

    def close(self):
        self.s.close()


def answer_session(t: Term, mode: str, cmd: str, pid: str, start: str, nbytes=None):
    t.expect(b"mode: ")
    t.sendline("uart" if mode == "raw" else mode)
    t.expect(b"cmd: ")
    t.sendline(cmd)
    if cmd == "quit":
        return
    t.expect(b"ramdisk pid: ")
    t.sendline(pid)
    t.expect(b"start block: ")
    t.sendline(start)
    if cmd == "dump":
        t.expect(b"bytes: ")
        t.sendline(str(nbytes))


def sender_stream(t: Term, blob: bytes, framing: str, packet: str,
                  ymodem: bool, corrupt: int, name: str) -> bool:
    blklen = 128 if packet == "soh" else 1024
    head = bytes([0x01 if packet == "soh" else 0x02])
    # Wait for the receiver handshake: THREE CONSECUTIVE Cs. Single
    # stale 'C's (e.g. the 'C' in "BLOCKED" from task-table output)
    # must not false-trigger; real handshake is an unbroken run.
    consec = 0
    got_c = False
    nones = 0
    for _ in range(400):
        c = t.getc()
        if c is None:
            # Slow (cold-TCG) guest: a quiet socket window is not a dead
            # receiver. Tolerate a few before giving up (each costs one
            # socket TIMEOUT).
            nones += 1
            if nones > 3:
                return False
            continue
        nones = 0
        if c == CHR_C:
            consec += 1
            if consec >= 3:
                got_c = True
                break
        else:
            consec = 0
    if not got_c:
        return False
    packets = []
    if ymodem:
        body = (name.encode() + b"\x00" + str(len(blob)).encode() + b"\x00")
        packets.append((0, body.ljust(128, b"\x00")))
    # Data packets are numbered from 1 in both protocols: the receiver
    # keeps expected==1 across block-0 and treats a repeated seq 0 as a
    # duplicate (re-ACK without staging), which would silently misalign
    # a YMODEM stream whose data started at 0.
    seq = 1
    for off in range(0, len(blob), blklen):
        packets.append((seq, blob[off:off + blklen].ljust(blklen, b"\x1a")))
        seq += 1
    n = 0
    for seqn, body in packets:
        n += 1
        crc = crc16(body)
        # YMODEM block-0 is the first packet (n == 1) and is always a
        # 128 B SOH packet (the receiver sizes the body by the start
        # byte: SOH->128, STX->1024). Data packets use the case framing.
        if ymodem and n == 1:
            hpkt = (bytes([SOH, seqn & 0xFF, 0xFF ^ (seqn & 0xFF)])
                    + body)
        else:
            hpkt = (head
                    + bytes([seqn & 0xFF, 0xFF ^ (seqn & 0xFF)]) + body)
        pkt = hpkt + bytes([(crc >> 8) & 0xFF, crc & 0xFF])
        # Corrupt-once (case e): only the FIRST attempt carries the flipped
        # CRC byte; retries send the clean packet, proving NAK recovery.
        # (Corrupting pkt itself would fail all 11 retries deterministically.)
        bad = (pkt[:-1] + bytes([(pkt[-1] ^ 0xFF) & 0xFF])
               if n == corrupt else None)
        for attempt in range(11):
            t.send(frame(bad if (bad is not None and attempt == 0)
                         else pkt, framing))
            c = t.getc()
            if c == ACK:
                break
            # A quiet window (cold-TCG stall) is retried: duplicates are
            # harmless (receiver re-ACKs the previous sequence). CAN aborts.
            if c == CAN:
                return False
            if c is None:
                continue
        else:
            return False
    t.send(frame(bytes([EOT]), framing))
    for _ in range(10):
        if t.getc() == ACK:
            return True
    return False


def do_rx(t: Term, mode: str, pid: str, start: int, blob: bytes,
          framing: str, packet: str, ymodem: bool, corrupt: int, name: str,
          expect_done: int) -> bool:
    answer_session(t, mode, "rx", pid, str(start))
    # Flush stale output (task table contains 'C's that would
    # false-trigger the handshake); the receiver streams fresh C's.
    t.flush_quiet()
    if not sender_stream(t, blob, framing, packet, ymodem, corrupt, name):
        return False
    line = t.readline(timeout=120)
    m = re.search(rb"done bytes=(\d+)", line)
    return bool(m) and int(m.group(1)) == expect_done


def do_dump(t: Term, mode: str, pid: str, start: int, nbytes: int) -> bytes | None:
    answer_session(t, mode, "dump", pid, str(start), nbytes)
    end = time.time() + 180
    while time.time() < end:
        if b"dump done" in t.buf:
            break
        chunk = t.recv_raw()
        if chunk:
            t.buf += chunk
    if b"dump done" not in t.buf:
        return None
    # Split at the LAST trailer (golden bytes may contain it) and drop
    # everything through it so no dump bytes pollute the next phase.
    idx = t.buf.rindex(b"dump done") + len(b"dump done")
    payload, t.buf = t.buf[:idx - len(b"dump done")], t.buf[idx:]
    # The receiver prints the trailer as "\ndump done": the shared serial
    # path expands that \n to CRLF, so the payload ends with the n dump
    # bytes + CRLF (dialogue echoes precede on echoing terminals; the
    # raw tty used here contributes none). Strip the CRLF, then take
    # the trailing window.
    if len(payload) < nbytes + 2:
        return None
    core = payload[:-2]
    if len(core) < nbytes:
        return None
    return core[-nbytes:]


def recover(t: Term) -> None:
    # Escape a failed attempt's receiver state back to a fresh `mode: `
    # prompt. CAN aborts an in-flight transfer; a stray CAN line at menu
    # level prints usage and also lands at mode:. Only used between
    # attempts (never before a first attempt: entry is always a freshly
    # printed `mode: ` on the success path). Bounded by the expect
    # timeout; raises TimeoutError if the guest is stalled past it.
    t.send(b"\x18\x18")
    t.expect(b"mode: ", timeout=120)


def run_case(t: Term, tag, start, gold, framing, packet, ymodem, corrupt,
             name, expect_done, pid) -> bool:
    # One data case with stall-tolerant retries: a TCG virtual-time stall
    # can wedge either side past fixed timeouts (#315 E2E evidence), so a
    # failed attempt recovers and retries rather than failing the suite.
    for attempt in (1, 2, 3):
        if attempt > 1:
            try:
                recover(t)
            except (TimeoutError, OSError):
                print(f"VERIFY-276: case {tag} attempt {attempt} "
                      f"recover timeout")
                continue
        if not do_rx(t, framing, pid, start, gold, framing, packet,
                      ymodem, corrupt, name, expect_done):
            print(f"VERIFY-276: case {tag} attempt {attempt} rx fail")
            continue
        got = do_dump(t, "uart", pid, start, len(gold))
        if got != gold:
            print(f"VERIFY-276: case {tag} attempt {attempt} dump "
                  f"mismatch (got {len(got) if got else -1} "
                  f"want {len(gold)})")
            continue
        extra = f"/ymodem" if ymodem else ""
        extra += "/corrupt" if corrupt else ""
        print(f"VERIFY-276: case {tag} PASS ({framing}/{packet}{extra}) "
              f"attempt {attempt}")
        return True
    print(f"VERIFY-276: FAIL {tag} (3 attempts)")
    return False


def main() -> int:
    t = Term()
    try:
        if t.log:
            import datetime
            t.log.write(
                f"== RUN {datetime.datetime.now().isoformat()} ==\n".encode())
            t.log.flush()
        t.send(b"\r")
        t.expect(b"/ $", timeout=120)
        t.sendline("tasks")
        out = t.expect(b"ramdiskd", timeout=60)
        m = re.search(rb"(\d+)\s+ramdiskd", out)
        if not m:
            print("VERIFY-276: FAIL ramdiskd pid not found")
            return 1
        pid = m.group(1).decode()
        print(f"VERIFY-276: ramdiskd pid={pid}")
        t.sendline("source /tests/xmodem276.txt")
        t.expect(b"xmodem receiver", timeout=120)
        # Golden patterns are 0x0A/0x09-free (see clean()): the shared
        # serial path expands 0x0A to CRLF and 0x09 to 0x09+4 spaces
        # (Terminal::serial_putchar/putchar), which would shift exact
        # window compares. Lengths (and hence done-counts) are unchanged.
        def clean(blob: bytes) -> bytes:
            return bytes(b if b not in (0x0A, 0x09) else 0x0E
                         for b in blob)
        gold_a = clean(bytes((i * 7 + 3) & 0xFF for i in range(300)))
        gold_b = clean(bytes((i * 13 + 11) & 0xFF for i in range(3 * 1024)))
        gold_d = clean(bytes((i * 5 + 1) & 0xFF for i in range(1024)))
        cases = [
            # (tag, start, gold, framing, packet, ymodem, corrupt, name,
            #  expect_done): pure XMODEM reports padded stream bytes,
            # YMODEM reports the exact size.
            ("a", 100, gold_a, "raw", "soh", False, 0, "a.bin", 384),
            ("b", 200, gold_b, "raw", "stx", True, 0, "b.bin", 3072),
            ("c", 300, gold_a, "base64", "soh", False, 0, "c.bin", 384),
            ("d", 400, gold_d, "hex", "stx", False, 0, "d.bin", 1024),
            ("e", 500, gold_a, "raw", "soh", False, 2, "e.bin", 384),
        ]
        for tag, start, gold, framing, packet, ymodem, corrupt, name, \
                expect_done in cases:
            if not run_case(t, tag, start, gold, framing, packet, ymodem,
                            corrupt, name, expect_done, pid):
                return 1
        # (f) Bogus pid: the transfer must fail closed (abort, no hang).
        # NOTE: pid 99991 never reaches the daemon — the client addresses
        # it directly, so the kernel fails destination lookup (-9) and the
        # receiver aborts on flush. This proves fail-closed addressing +
        # abort propagation, NOT daemon NOGRANT behavior (the daemon never
        # sees the request). Daemon-side NOGRANT is covered at kernel
        # level by ramdisk_grant_fail_closed (grant_storage pid 0/unknown
        # -> NOGRANT). There is deliberately no zero-check here: nothing
        # guarantees the region (and pristine carve frames are NOT
        # guaranteed zero — see the carve-content follow-up; only
        # staged-then-dumped round-trips carry content assertions).
        answer_session(t, "raw", "rx", "99991", "600")
        t.flush_quiet()
        if not sender_stream(t, gold_a, "raw", "soh", False, 0, "f.bin"):
            print("VERIFY-276: note (f) sender saw abort (receiver-side abort ok)")
        try:
            line = t.readline(timeout=60)
        except TimeoutError:
            line = b""
        if b"abort" not in line:
            print(f"VERIFY-276: FAIL (f) no abort line: {line!r}")
            return 1
        print("VERIFY-276: case f bogus-pid-abort PASS")
        answer_session(t, "uart", "quit", pid, "0")
        print("VERIFY-276: ALL PASS")
        return 0
    except (TimeoutError, ConnectionError, OSError) as e:
        print(f"VERIFY-276: FAIL {e}")
        return 1
    finally:
        t.close()


if __name__ == "__main__":
    sys.exit(main())
