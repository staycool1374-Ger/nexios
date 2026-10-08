#!/usr/bin/env python3
"""Host-side XMODEM/YMODEM sender for #276 E2E (tools/verify-276.exp).

Usage: xmodem_send.py HOST PORT FILE [--framing raw|base64|hex]
                       [--packet soh|stx] [--ymodem] [--corrupt N]
                       [--start-block N] [--timeout S]

Waits for the receiver's 'C', streams the file, handles ACK/NAK/EOT.
--corrupt N flips one CRC byte of packet N (1-based) to exercise the
receiver's NAK path once. Exit 0 on clean completion, 1 on abort.
"""
import socket
import sys
import base64
import binascii

SOH, STX, EOT, ACK, NAK, CAN, CHR_C = 0x01, 0x02, 0x04, 0x06, 0x15, 0x18, 0x43


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
    if framing == "hex":
        return binascii.hexlify(payload) + b"\n"
    raise ValueError(framing)


class Sender:
    def __init__(self, host, port, timeout):
        self.s = socket.create_connection((host, port), timeout=timeout)
        self.s.settimeout(timeout)
        self.buf = b""

    def getc(self):
        while not self.buf:
            chunk = self.s.recv(4096)
            if not chunk:
                return None
            self.buf += chunk
        c, self.buf = self.buf[0], self.buf[1:]
        return c

    def put(self, data: bytes, framing: str):
        self.s.sendall(frame(data, framing))

    def close(self):
        self.s.close()


def main() -> int:
    args = sys.argv[1:]
    framing, packet, ymodem, corrupt = "raw", "stx", False, 0
    timeout = 30
    pos = []
    i = 0
    while i < len(args):
        if args[i] == "--framing":
            framing = args[i + 1]
            i += 2
        elif args[i] == "--packet":
            packet = args[i + 1]
            i += 2
        elif args[i] == "--ymodem":
            ymodem = True
            i += 1
        elif args[i] == "--corrupt":
            corrupt = int(args[i + 1])
            i += 2
        elif args[i] == "--timeout":
            timeout = int(args[i + 1])
            i += 2
        else:
            pos.append(args[i])
            i += 1
    if len(pos) != 3:
        print("usage: HOST PORT FILE [--framing ..] [--packet soh|stx] [--ymodem] [--corrupt N]")
        return 2
    host, port, path = pos[0], int(pos[1]), pos[2]
    with open(path, "rb") as f:
        blob = f.read()
    blklen = 128 if packet == "soh" else 1024
    head = bytes([0x01 if packet == "soh" else 0x02])
    snd = Sender(host, port, timeout)
    try:
        # Wait for receiver 'C' handshake.
        for _ in range(40):
            c = snd.getc()
            if c is None:
                print("SENDER: EOF waiting C")
                return 1
            if c == CHR_C:
                break
        else:
            print("SENDER: no C received")
            return 1
        packets = []
        if ymodem:
            name = path.encode() + b"\x00"
            size = str(len(blob)).encode() + b"\x00"
            body = (name + size).ljust(128, b"\x00")
            packets.append((0, body))
        for off in range(0, len(blob), blklen):
            chunk = blob[off:off + blklen].ljust(blklen, b"\x1a")
            packets.append((len(packets) + (0 if ymodem else 1), chunk))
        n = 0
        for seq, body in packets:
            n += 1
            seqb = seq & 0xFF
            crc = crc16(body)
            pkt = head + bytes([seqb, 0xFF ^ seqb]) + body
            pkt += bytes([(crc >> 8) & 0xFF, crc & 0xFF])
            if n == corrupt:
                pkt = pkt[:-1] + bytes([(pkt[-1] ^ 0xFF) & 0xFF])
            for attempt in range(11):
                snd.put(pkt, framing)
                c = snd.getc()
                if c == ACK:
                    break
                if c is None or c == CAN:
                    print("SENDER: abort during packet", n)
                    return 1
            else:
                print("SENDER: retries exhausted packet", n)
                return 1
        snd.put(bytes([EOT]), framing)
        for _ in range(5):
            c = snd.getc()
            if c == ACK:
                print(f"SENDER: done packets={n} bytes={len(blob)}")
                return 0
        print("SENDER: no ACK to EOT")
        return 1
    finally:
        snd.close()


if __name__ == "__main__":
    sys.exit(main())
