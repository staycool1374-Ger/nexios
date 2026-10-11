#!/usr/bin/env python3
"""One-command host upload of a file into ramdisk (issue #313).

Attaches to a running target over TCP serial (like tools/verify-276.py),
drives the in-tree xmodem receiver (`rx` cmd) and streams the file with a
YMODEM block-0 (name + exact size, so the receiver reports `done bytes=N`
with N == file size). Framing is transport-only: the receiver decodes it
before staging, so the stored bytes are always the raw file bytes.

Usage: upload-ramdisk.py FILE --start-block N [--pid N] [--port P]
                              [--packet soh|stx] [--framing auto|raw|base64|hex]
                              [--timeout S] [--selftest]

Progress (both ends): host prints a single updating `\\r` line with
percentage + throughput + ETA; the receiver prints `[rx n=.. total=..]`
every 16 blocks (userspace/xmodem.c `RX_PROGRESS_BLOCKS`). Receiver text
shares the serial channel with ACK/NAK/CAN bytes, so the sender applies a
byte-filter discipline: ACK advances, CAN aborts, NAK resends, anything
else is discarded spill (bounded) without consuming an attempt.

Exit codes: 0 ok, 1 dialogue/handshake failure, 2 usage/byte-count
mismatch, 3 receiver CAN-abort, 4 timeout/retry-exhausted.
"""
import importlib.util
import os
import re
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
_spec = importlib.util.spec_from_file_location(
    "verify276", os.path.join(HERE, "verify-276.py"))
assert _spec is not None and _spec.loader is not None
verify = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(verify)

ACK, NAK, CAN, CHR_C = 0x06, 0x15, 0x18, 0x43
EOT = 0x04
SPILL_CAP = 65536
RAMDISK_BLOCKS = 32768
RAMDISK_BLOCK_SIZE = 512


def select_framing(data: bytes) -> str:
    """auto rule: 0x0A/0x09 bytes force base64 (shared serial expands
    LF->CRLF and TAB->spaces, corrupting raw binary); else raw."""
    for b in data:
        if b == 0x0A or b == 0x09:
            return "base64"
    return "raw"


def parse_ramdisk_pid(tasks_output: bytes) -> str | None:
    """Extract the ramdiskd pid from `tasks` output (`<pid> ramdiskd`)."""
    m = re.search(rb"(\d+)\s+ramdiskd", tasks_output)
    return m.group(1).decode() if m else None


def format_progress(sent: int, total: int, elapsed: float,
                    naks: int, spill: int) -> str:
    """Single updating host progress line (no newline; caller adds \\r)."""
    pct = (sent * 100 // total) if total else 100
    rate = (sent / elapsed) if elapsed > 0 else 0.0
    eta = ((total - sent) / rate) if rate > 0 and total > sent else 0.0
    return ("[up %d/%d %d%% %.1f B/s eta=%.0fs naks=%d spill=%d]"
            % (sent, total, pct, rate, eta, naks, spill))


def ymodem_block0(name: bytes, size: int) -> bytes:
    """YMODEM block-0 body: name NUL size-ascii NUL, 128 B NUL-padded."""
    body = name + b"\x00" + str(size).encode() + b"\x00"
    return body.ljust(128, b"\x00")


def wait_handshake(t, limit: int = 400, stats=None) -> bool:
    """Wait for THREE CONSECUTIVE Cs (single stale Cs must not trigger).

    stats (optional dict) receives `seen_c`, `seen_other`, `nones` counts
    for failure diagnosis (receiver C-window is ~30 s; a late waiter
    misses it — retry, don't chase).
    """
    consec = 0
    nones = 0
    seen_c = 0
    seen_other = 0
    for _ in range(limit):
        c = t.getc()
        if c is None:
            nones += 1
            if nones > 3:
                break
            continue
        nones = 0
        if c == CHR_C:
            seen_c += 1
            consec += 1
            if consec >= 3:
                return True
        else:
            seen_other += 1
            consec = 0
    if stats is not None:
        stats.update(seen_c=seen_c, seen_other=seen_other, nones=nones)
    return False


def send_packets(t, blob: bytes, framing: str, packet: str, name: bytes,
                 progress) -> tuple:
    """Stream blob with the byte-filter discipline.

    Returns (ok, naks, spill, sent). ok True only when every packet
    (block-0 + data) plus EOT was ACKed.
    """
    blklen = 128 if packet == "soh" else 1024
    head = bytes([0x01 if packet == "soh" else 0x02])
    packets = [(0, ymodem_block0(name, len(blob)))]
    seq = 1
    for off in range(0, len(blob), blklen):
        chunk = blob[off:off + blklen]
        if len(chunk) < blklen:
            chunk = chunk.ljust(blklen, b"\x1a")
        packets.append((seq, chunk))
        seq += 1
    naks = 0
    spill = 0
    sent = 0
    total_stream = sum(len(b) for _, b in packets)
    for seqn, body in packets:
        if packet == "soh" or seqn == 0:
            hpkt = (bytes([0x01, seqn & 0xFF, 0xFF ^ (seqn & 0xFF)])
                    + body)
        else:
            hpkt = (head + bytes([seqn & 0xFF, 0xFF ^ (seqn & 0xFF)])
                    + body)
        crc = verify.crc16(body)
        pkt = hpkt + bytes([(crc >> 8) & 0xFF, crc & 0xFF])
        acked = False
        for _ in range(11):
            t.send(verify.frame(pkt, framing))
            while True:
                c = t.getc()
                if c is None:
                    break  # quiet window: resend (bounded by attempts)
                if c == ACK:
                    sent += len(body) if seqn else 0
                    progress(sent, naks, spill)
                    acked = True
                    break
                if c == CAN:
                    t.getc()  # consume second CAN
                    return (False, naks, spill, sent)
                if c == NAK:
                    naks += 1
                    break  # resend same packet immediately
                spill += 1  # progress text / stray bytes: discard
                if spill > SPILL_CAP:
                    return (False, naks, spill, sent)
                continue
            if acked:
                break
        if not acked:
            return (False, naks, spill, sent)
    # EOT with the same filter (up to 3 sends).
    for _ in range(3):
        t.send(verify.frame(bytes([EOT]), framing))
        for _ in range(64):
            c = t.getc()
            if c is None:
                break
            if c == ACK:
                return (True, naks, spill, sent)
            if c == CAN:
                return (False, naks, spill, sent)
            if c == NAK:
                break
            spill += 1
            if spill > SPILL_CAP:
                return (False, naks, spill, sent)
    return (False, naks, spill, sent)


def selftest() -> int:
    """Host-side checks without a target (pure functions + filter sim)."""
    fails = 0

    def check(tag, cond):
        nonlocal fails
        print(("SELFTEST %s: %s" % (tag, "PASS" if cond else "FAIL")))
        if not cond:
            fails += 1

    check("framing-raw", select_framing(b"\x00\x01\x7f\x80") == "raw")
    check("framing-lf", select_framing(b"a\nb") == "base64")
    check("framing-tab", select_framing(b"a\tb") == "base64")
    check("framing-empty", select_framing(b"") == "raw")
    sample = b"  12 ramdiskd prio 20\n  13 xmodem prio 2\n"
    check("pid-parse", parse_ramdisk_pid(sample) == "12")
    check("pid-miss", parse_ramdisk_pid(b"no daemons") is None)
    line = format_progress(512, 1024, 2.0, 1, 3)
    check("progress-fmt", line == "[up 512/1024 50% 256.0 B/s eta=2s"
          " naks=1 spill=3]")
    b0 = ymodem_block0(b"f.bin", 300)
    check("block0-len", len(b0) == 128)
    check("block0-fmt", b0.startswith(b"f.bin\x00300\x00"))

    class FakeTerm:
        """Scripted receiver: 3 Cs, ACKs with progress spill, EOT ACK."""

        def __init__(self, script):
            self.script = list(script)
            self.sent = []

        def getc(self):
            if not self.script:
                return None
            return self.script.pop(0)

        def send(self, data):
            self.sent.append(data)

    spill_line = b"\n[rx n=8192 total=9216]\n"
    # 1 data packet of 128 B + block-0 (send_packets starts post-handshake):
    # ACK, spill bytes, ACK, EOT, ACK.
    blob = bytes([i & 0xFF for i in range(100)])
    script = [ACK] + list(spill_line) + [ACK, ACK]
    fake = FakeTerm(script)
    seen = []
    ok, naks, spill, sent = send_packets(
        fake, blob, "raw", "soh", b"t.bin",
        lambda s, n, p: seen.append((s, n, p)))
    check("filter-ok", ok is True)
    check("filter-spill", spill == len(spill_line))
    check("filter-naks", naks == 0)
    check("filter-sent", sent == 128)
    check("filter-progress-cb", len(seen) == 2)
    # NAK injection: exactly one same-packet resend, then ACKs.
    script2 = [NAK, ACK, ACK, ACK]
    fake2 = FakeTerm(script2)
    ok2, naks2, spill2, _ = send_packets(
        fake2, blob, "raw", "soh", b"t.bin", lambda s, n, p: None)
    check("nak-retry-ok", ok2 is True)
    check("nak-count", naks2 == 1)
    check("nak-resend", len(fake2.sent) == 4)  # b0, pkt1, pkt1, EOT
    return 1 if fails else 0


def usage() -> int:
    sys.stderr.write(
        "usage: upload-ramdisk.py FILE --start-block N [--pid N]\n"
        "                          [--port P] [--packet soh|stx]\n"
        "                          [--framing auto|raw|base64|hex]\n"
        "                          [--timeout S]\n"
        "  FILE is staged at ramdisk block N (required: no silent default\n"
        "  overwrites live ramdisk data). Needs a running target with\n"
        "  bin/xmodem.c.elf active via `source /tests/xmodem276.txt`.\n")
    return 2


def main(argv) -> int:
    if "--selftest" in argv:
        return selftest()
    pos = []
    start = None
    pid = None
    port = 4465
    packet = "stx"
    framing = "auto"
    timeout = 120
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--start-block" and i + 1 < len(argv):
            start, i = int(argv[i + 1]), i + 2
        elif a == "--pid" and i + 1 < len(argv):
            pid, i = argv[i + 1], i + 2
        elif a == "--port" and i + 1 < len(argv):
            port, i = int(argv[i + 1]), i + 2
        elif a == "--packet" and i + 1 < len(argv):
            packet, i = argv[i + 1], i + 2
        elif a == "--framing" and i + 1 < len(argv):
            framing, i = argv[i + 1], i + 2
        elif a == "--timeout" and i + 1 < len(argv):
            timeout, i = int(argv[i + 1]), i + 2
        elif a.startswith("-"):
            return usage()
        else:
            pos.append(a)
            i += 1
    if len(pos) != 1 or start is None:
        return usage()
    if packet not in ("soh", "stx"):
        sys.stderr.write("upload: --packet must be soh|stx\n")
        return 2
    if framing not in ("auto", "raw", "base64", "hex"):
        sys.stderr.write("upload: --framing must be auto|raw|base64|hex\n")
        return 2
    if start < 0 or start >= RAMDISK_BLOCKS:
        sys.stderr.write("upload: --start-block out of range\n")
        return 2
    path = pos[0]
    try:
        with open(path, "rb") as f:
            blob = f.read()
    except OSError as e:
        sys.stderr.write("upload: cannot read %s: %s\n" % (path, e))
        return 2
    if not blob:
        sys.stderr.write("upload: refusing empty file (receiver NAKs"
                         " zero-size YMODEM)\n")
        return 2
    capacity = (RAMDISK_BLOCKS - start) * RAMDISK_BLOCK_SIZE
    if len(blob) > capacity:
        sys.stderr.write("upload: file (%d B) exceeds ramdisk capacity"
                         " from block %d (%d B)\n"
                         % (len(blob), start, capacity))
        return 2
    if framing == "auto":
        framing = select_framing(blob)
    # Framing is transport-only: the receiver decodes it before staging,
    # so stored bytes are always the raw file bytes.
    name = os.path.basename(path).encode() or b"upload.bin"
    t = verify.Term.__new__(verify.Term)
    import socket
    t.s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
    t.s.settimeout(timeout)
    t.buf = b""
    t.log = None
    t0 = [0.0]

    def progress(sent, naks, spill):
        el = time.time() - t0[0]
        sys.stdout.write("\r" + format_progress(sent, len(blob), el,
                                                naks, spill))
        sys.stdout.flush()

    try:
        t.send(b"\r")
        try:
            t.expect(b"/ $", timeout=timeout)
        except TimeoutError:
            sys.stderr.write("upload: no shell prompt (target up?)\n")
            return 1
        if pid is None:
            t.sendline("tasks")
            try:
                out = t.expect(b"ramdiskd", timeout=60)
            except TimeoutError:
                sys.stderr.write("upload: ramdiskd not running"
                                 " (NOGRANT: nothing written)\n")
                return 1
            pid = parse_ramdisk_pid(out)
            if pid is None:
                sys.stderr.write("upload: ramdiskd pid not found\n")
                return 1
        sys.stderr.write("upload: ramdiskd pid=%s start=%d framing=%s\n"
                         % (pid, start, framing))
        t.sendline("source /tests/xmodem276.txt")
        try:
            t.expect(b"xmodem receiver", timeout=timeout)
        except TimeoutError:
            sys.stderr.write("upload: xmodem receiver not started\n")
            return 1
        mode_word = {"raw": "uart", "base64": "base64",
                     "hex": "hex"}[framing]
        verify.answer_session(t, mode_word, "rx", pid, str(start))
        t.flush_quiet()
        t0[0] = time.time()
        if not wait_handshake(t):
            sys.stderr.write("upload: no receiver handshake (C)\n")
            return 1
        ok, naks, spill, sent = send_packets(
            t, blob, framing, packet, name, progress)
        el = time.time() - t0[0]
        if not ok:
            sys.stdout.write("\n")
            sys.stderr.write("upload: receiver CAN-aborted (check grant/"
                             "serial log; receiver writes nothing further;"
                             " NOGRANT leaves zero writes)\n")
            return 3
        try:
            line = t.readline(timeout=timeout)
        except TimeoutError:
            sys.stderr.write("upload: no `done bytes=` line (timeout)\n")
            return 4
        m = re.search(rb"done bytes=(\d+)", line)
        if not m or int(m.group(1)) != len(blob):
            sys.stderr.write("upload: byte-count mismatch: %r\n" % (line,))
            return 2
        blocks = (len(blob) + RAMDISK_BLOCK_SIZE - 1) // RAMDISK_BLOCK_SIZE
        sys.stdout.write("\nupload done bytes=%d blocks=%d framing=%s"
                         " naks=%d spill=%d time=%.1fs\n"
                         % (len(blob), blocks, framing, naks, spill, el))
        return 0
    except (TimeoutError, ConnectionError, OSError) as e:
        sys.stderr.write("upload: FAIL %s\n" % (e,))
        return 4
    finally:
        try:
            t.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
